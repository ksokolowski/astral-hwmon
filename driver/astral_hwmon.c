// SPDX-License-Identifier: GPL-2.0-only
/* driver/astral_hwmon.c - module lifecycle and the hwmon surface.
 *
 * Read-only by construction: no write callback is implemented and is_visible
 * returns 0444 unconditionally. Writing to the wrong device on a GPU I2C bus can
 * destroy hardware, so the capability is left out of the code entirely rather
 * than guarded at runtime.
 */

#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>

#include "astral.h"

/*
 * Cache TTL, and therefore a ceiling on how often we touch the I2C bus: one
 * 24-byte block read costs ~5 ms measured, so 200 ms is at most ~2.5% bus duty
 * even under continuous hammering, and it collapses a `sensors` run's twelve
 * attribute reads into one transaction.
 *
 * Shorter than the classic hwmon idiom (w83781d uses 1.5 s) and than lm90's
 * 500 ms, deliberately: those exist for chips whose own conversion is slow,
 * whereas this one converts continuously. Keeping it well under a second is
 * what leaves room for a future guard to poll at a rate where a fast
 * over-current is still catchable.
 */
#define ASTRAL_CACHE_TTL_MS	200
#define ASTRAL_CACHE_TTL	msecs_to_jiffies(ASTRAL_CACHE_TTL_MS)

static int bus = -1;
module_param(bus, int, 0444);
MODULE_PARM_DESC(bus, "Pin which of the card's i2c adapters to use; the PCI-id check still applies (default: auto)");

static int addr = ASTRAL_DEFAULT_ADDR;
module_param(addr, int, 0444);
MODULE_PARM_DESC(addr, "Chip address (default: 0x2b)");

static bool allow_unknown;
module_param(allow_unknown, bool, 0444);
MODULE_PARM_DESC(allow_unknown, "Skip the PCI subsystem-id allowlist (default: N)");

struct astral_data {
	struct i2c_client *client;
	struct mutex lock;		/* guards the cache below */
	struct astral_frame frame;
	unsigned long last_updated;
	bool valid;
};

/*
 * Probing is deferred to a workqueue rather than done inline in the notifier.
 * BUS_NOTIFY_ADD_DEVICE fires from inside device_add(), with the driver core
 * holding the device lock and the adapter not yet able to carry a transfer:
 * measured at boot, the read fails with -EIO ~1.4 s before the same read
 * succeeds by hand. Retrying from process context covers however long the GPU
 * takes to become ready.
 */
#define ASTRAL_RETRY_DELAY_MS	2000
#define ASTRAL_RETRY_LIMIT	15

static void astral_probe_worker(struct work_struct *work);
static DECLARE_DELAYED_WORK(astral_probe_work, astral_probe_worker);
static const char *astral_model = "ASUS ROG Astral";

/*
 * Both guarded by astral_lock: the worker increments, the notifier resets, and
 * they run concurrently. astral_stage_reported keeps the "still waiting" line
 * to one per retry cycle - the attempt counter cannot do that job on its own,
 * because module_init performs a probe pass of its own before the worker's
 * first.
 */
static int astral_attempts;
static bool astral_stage_reported;

/*
 * astral_client is the single claimed chip, written under astral_lock. The
 * probe worker and module exit both touch it, so the claim is not a plain
 * check-then-set.
 */
static DEFINE_MUTEX(astral_lock);
static struct i2c_client *astral_client;	/* published chip, NULL if none */
static bool astral_claiming;			/* an attach is in flight */
static struct notifier_block astral_nb;

/*
 * One block read serves all twelve channels; without this a single `sensors`
 * run would issue twelve separate transactions.
 *
 * Decodes into a caller-owned frame and only commits to the cache once the
 * data has passed the plausibility check. Decoding straight into the cache
 * would let a failed read publish its zeros to a concurrent reader that had
 * already been handed a cache hit - which is exactly the all-zeros reading
 * this driver exists to never emit.
 */
static int astral_refresh_locked(struct astral_data *data)
{
	struct astral_frame fresh;
	int ret;

	lockdep_assert_held(&data->lock);

	if (data->valid &&
	    time_before(jiffies, data->last_updated + ASTRAL_CACHE_TTL))
		return 0;

	ret = astral_read_frame(data->client, &fresh);
	if (ret) {
		data->valid = false;
		return ret;
	}

	if (!astral_frame_plausible(&fresh)) {
		data->valid = false;
		/* -EPROTO distinguishes "the chip answered with nonsense" from a
		 * transport failure, which also surfaces as -EIO. Only the attach
		 * path cares; astral_read() maps it back to -EIO for userspace.
		 */
		return -EPROTO;
	}

	data->frame = fresh;
	data->last_updated = jiffies;
	data->valid = true;

	return 0;
}

/* Convenience for callers outside the read path that just need a probe. */
static int astral_refresh(struct astral_data *data)
{
	int ret;

	mutex_lock(&data->lock);
	ret = astral_refresh_locked(data);
	mutex_unlock(&data->lock);

	return ret;
}

static umode_t astral_is_visible(const void *drvdata, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	return 0444;	/* read-only, structurally - never make this writable */
}

static int astral_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *val)
{
	struct astral_data *data = dev_get_drvdata(dev);
	int ret;

	/* Tells userspace how fresh a reading can possibly be, so tools do not
	 * have to guess a poll rate. Read-only: see is_visible().
	 */
	if (type == hwmon_chip) {
		if (attr != hwmon_chip_update_interval)
			return -EOPNOTSUPP;
		*val = ASTRAL_CACHE_TTL_MS;
		return 0;
	}

	if (channel < 0 || channel >= ASTRAL_PIN_COUNT)
		return -EINVAL;

	/* Refresh and copy under one lock. Dropping it in between would let
	 * another reader invalidate the cache before this value is taken.
	 */
	mutex_lock(&data->lock);

	ret = astral_refresh_locked(data);
	if (ret) {
		if (ret == -EPROTO)
			ret = -EIO;	/* userspace wants the conventional code */
		goto out;
	}

	switch (type) {
	case hwmon_in:
		*val = data->frame.millivolts[channel];
		break;
	case hwmon_curr:
		*val = data->frame.milliamps[channel];
		break;
	default:
		ret = -EOPNOTSUPP;
	}

out:
	mutex_unlock(&data->lock);
	return ret;
}

static const char *const astral_in_labels[] = {
	"12VHPWR Pin1 Voltage", "12VHPWR Pin2 Voltage", "12VHPWR Pin3 Voltage",
	"12VHPWR Pin4 Voltage", "12VHPWR Pin5 Voltage", "12VHPWR Pin6 Voltage",
};

static const char *const astral_curr_labels[] = {
	"12VHPWR Pin1 Current", "12VHPWR Pin2 Current", "12VHPWR Pin3 Current",
	"12VHPWR Pin4 Current", "12VHPWR Pin5 Current", "12VHPWR Pin6 Current",
};

static int astral_read_string(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, const char **str)
{
	if (channel < 0 || channel >= ASTRAL_PIN_COUNT)
		return -EINVAL;

	switch (type) {
	case hwmon_in:
		*str = astral_in_labels[channel];
		return 0;
	case hwmon_curr:
		*str = astral_curr_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info *const astral_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(in,
		HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
		HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_ops astral_hwmon_ops = {
	.is_visible = astral_is_visible,
	.read = astral_read,
	.read_string = astral_read_string,
};

static const struct hwmon_chip_info astral_chip_info = {
	.ops = &astral_hwmon_ops,
	.info = astral_info,
};

/*
 * Everything allocated here is devm-scoped to the i2c client, so when the
 * client goes away - including when i2c_del_adapter() tears it down behind
 * our back on `rmmod nvidia` - the hwmon device, the data and the mutex all
 * go with it. Only the astral_client pointer needs clearing by hand, which
 * the notifier does.
 *
 * Returns the claimed client, or NULL. MUST NOT be called with astral_lock
 * held: the failure path unregisters the client, which fires DEL_DEVICE back
 * into the notifier, which takes that same lock.
 *
 * Publishes astral_client itself, as soon as the device exists rather than on
 * the way out. i2c_del_adapter() frees every non-dummy child client, so a
 * removal landing mid-attach must find a pointer the DEL notifier can clear;
 * publishing afterwards left a window in which astral_client was set to memory
 * that had already been freed, and module exit would then unregister it.
 */
static struct i2c_client *astral_attach(struct i2c_adapter *adap, const char *model,
					int attempt)
{
	struct i2c_board_info info = { I2C_BOARD_INFO(ASTRAL_CHIP_NAME, 0) };
	struct astral_data *data;
	struct i2c_client *client;
	struct device *hwmon;
	int ret;

	info.addr = addr;

	client = i2c_new_client_device(adap, &info);
	if (IS_ERR(client)) {
		/* Worth saying out loud: -EBUSY here means something else already
		 * owns this address, which is a different problem from the chip
		 * not answering, and the retry path's message would misattribute
		 * it to the card.
		 */
		if (attempt == 0)
			dev_warn(&adap->dev,
				 "astral-hwmon: cannot claim 0x%02x on %s (%ld)\n",
				 addr, adap->name, PTR_ERR(client));
		return NULL;
	}

	mutex_lock(&astral_lock);
	astral_client = client;
	mutex_unlock(&astral_lock);

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		goto err_unregister;

	data->client = client;
	if (devm_mutex_init(&client->dev, &data->lock))
		goto err_unregister;

	/* Prove the chip answers with real data before registering anything.
	 * This is what rejects the silent all-zeros frame.
	 */
	ret = astral_refresh(data);
	if (ret) {
		/* Distinguish "nothing answered" from "answered with garbage" -
		 * the only distinction that matters when triaging a new SKU.
		 */
		/* Quiet on retries: at boot this fires repeatedly while the GPU
		 * finishes coming up, and 15 identical lines help nobody. The
		 * count is passed in rather than read from the global, which the
		 * notifier can reset underneath us.
		 */
		if (attempt == 0)
			dev_info(&adap->dev,
				 "astral-hwmon: %s at 0x%02x %s (%d), will retry\n",
				 model, addr,
				 ret == -EPROTO ? "returned an implausible frame"
						: "did not respond",
				 ret);
		else
			dev_dbg(&adap->dev,
				"astral-hwmon: attempt %d at 0x%02x failed (%d)\n",
				attempt, addr, ret);
		goto err_unregister;
	}

	hwmon = devm_hwmon_device_register_with_info(&client->dev, ASTRAL_CHIP_NAME,
						     data, &astral_chip_info, NULL);
	if (IS_ERR(hwmon))
		goto err_unregister;

	/* No i2c_set_clientdata(): nothing reads it back. hwmon holds `data` as
	 * the drvdata of its own device, and no i2c_driver is ever bound here.
	 */
	dev_info(&adap->dev, "astral-hwmon: %s - six 12VHPWR pins on %s\n",
		 model, adap->name);

	return client;

err_unregister:
	/* Unpublish first, then unregister with the lock DROPPED - the DEL
	 * notification i2c_unregister_device() fires re-enters astral_lock.
	 * The compare matters: the notifier may already have cleared it.
	 */
	mutex_lock(&astral_lock);
	if (astral_client == client)
		astral_client = NULL;
	mutex_unlock(&astral_lock);

	i2c_unregister_device(client);
	return NULL;
}

/*
 * The bus=/adapter-name half of the filter. Kept apart from the PCI gate
 * because the scan counts adapters that pass the gate but fail this one, which
 * is what distinguishes "the card has no adapter 1 yet" from "no card".
 */
static bool astral_adapter_selected(struct i2c_adapter *adap)
{
	if (bus >= 0)
		return i2c_adapter_id(adap) == bus;

	return astral_is_sensor_adapter(adap);
}

struct astral_probe_ctx {
	struct astral_scan scan;
	int adapter_id;		/* the chosen bus, or -1 */
};

/*
 * Runs under the i2c core lock, held by i2c_for_each_dev() across the whole
 * walk. Therefore: inspection only. No I2C traffic, no device registration,
 * and no i2c_get_adapter() - that takes the same lock and would self-deadlock.
 *
 * Stops the walk at the first candidate. The counters are only ever consulted
 * when no candidate was found, so an early stop never truncates a count that
 * gets used.
 */
static int astral_try_adapter(struct device *dev, void *data)
{
	struct astral_probe_ctx *ctx = data;
	struct i2c_adapter *adap;
	const char *model = NULL;

	if (dev->type != &i2c_adapter_type)
		return 0;

	adap = to_i2c_adapter(dev);

	/* PCI id first, always. Nothing below this line may put traffic on a
	 * bus that does not belong to a recognised card - not even bus=.
	 * This is pure inspection of the device tree; no I2C happens here.
	 */
	if (!astral_card_matches(adap, allow_unknown, &model))
		return 0;

	ctx->scan.card_adapters++;

	if (!astral_adapter_selected(adap))
		return 0;

	ctx->scan.candidates++;
	ctx->adapter_id = i2c_adapter_id(adap);

	return 1;	/* stop the walk; the attach happens outside it */
}

/*
 * One full probe pass: find a candidate adapter, then attach to it.
 *
 * The walk and the attach are deliberately separate calls. i2c_for_each_dev()
 * holds the i2c core lock for the duration of the callback, and attaching does
 * real bus traffic plus client registration - which under that lock would block
 * every i2c_add_adapter(), i2c_del_adapter() and i2c_get_adapter() on the
 * machine. At boot that is not a brief stall: the chip is not answering yet, so
 * each attempt burns the adapter timeout (~1.4 s of failing reads, measured).
 * It also invites an AB-BA deadlock against the nvidia driver, which registers
 * its adapters from paths of its own.
 *
 * So the callback only picks a bus number, and everything that can block runs
 * here. Taking the reference by number through i2c_get_adapter() rather than
 * carrying the pointer out is deliberate: it pins the adapter's owner module
 * too, so `rmmod nvidia` cannot complete underneath an attach in progress.
 */
static void astral_probe_once(struct astral_scan *scan)
{
	struct astral_probe_ctx ctx = { .adapter_id = -1 };
	struct i2c_client *client = NULL;
	struct i2c_adapter *adap;
	const char *model = NULL;
	int attempt;

	mutex_lock(&astral_lock);
	if (astral_client || astral_claiming) {
		mutex_unlock(&astral_lock);
		return;
	}
	astral_claiming = true;
	attempt = astral_attempts;
	mutex_unlock(&astral_lock);

	i2c_for_each_dev(&ctx, astral_try_adapter);
	*scan = ctx.scan;

	adap = ctx.adapter_id >= 0 ? i2c_get_adapter(ctx.adapter_id) : NULL;
	if (adap) {
		/* Re-check the gate. The number was chosen under the core lock,
		 * but that lock is dropped before we get here and an i2c id is
		 * reusable, so the bus now answering to it is not provably the
		 * one that passed. Attaching without checking again would put
		 * traffic on a foreign bus - the one thing this driver must
		 * never do.
		 */
		if (astral_card_matches(adap, allow_unknown, &model) &&
		    astral_adapter_selected(adap))
			client = astral_attach(adap, model, attempt);
	}

	/* astral_client is published by astral_attach() itself, early enough
	 * for the DEL notifier to clear it. Only the claim flag is settled
	 * here - re-storing the client would reinstate a pointer the notifier
	 * may have just cleared because the adapter went away.
	 */
	mutex_lock(&astral_lock);
	astral_claiming = false;
	/* Counted here rather than in the worker so module_init's synchronous
	 * pass counts too - otherwise its failure log repeats on the worker's
	 * first run, which read as two separate failures.
	 */
	if (!client)
		astral_attempts++;
	mutex_unlock(&astral_lock);

	/* Released only once the claim is settled. While it is held the
	 * adapter's owner cannot be unloaded, and `rmmod nvidia` - which is
	 * what deletes these adapters - is the path that would otherwise free
	 * our client from under the attach.
	 */
	if (adap)
		i2c_put_adapter(adap);
}

/*
 * Adapters come and go with the nvidia module and with GPU resets. ADD is how
 * we attach; DEL is how we notice our chip has been taken away. Without the
 * DEL case astral_client would dangle at the moment i2c_del_adapter() frees
 * our client, and the stale non-NULL pointer would also block re-attaching
 * when the adapters come back.
 */
static int astral_bus_notify(struct notifier_block *nb, unsigned long action, void *arg)
{
	struct device *dev = arg;
	bool rearm = false;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		/* Never attach from here - see the comment on astral_probe_work.
		 * Each of the card's adapters appearing pushes the timer out, so
		 * the probe runs once the batch has settled.
		 *
		 * The dev->type filter MUST come before astral_lock: attaching
		 * calls i2c_new_client_device(), whose own ADD notification
		 * arrives here, and taking the lock first would self-deadlock.
		 */
		if (dev->type != &i2c_adapter_type)
			break;

		/* Only adapters on a card we would actually attach to. Any i2c
		 * adapter anywhere in the machine used to restart the whole retry
		 * cycle - a monitor hotplug creating a DisplayPort DDC bus was
		 * enough - including long after a successful attach.
		 */
		if (!astral_card_matches(to_i2c_adapter(dev), allow_unknown, NULL))
			break;

		mutex_lock(&astral_lock);
		if (!astral_client) {
			astral_attempts = 0;
			astral_stage_reported = false;
			rearm = true;
		}
		mutex_unlock(&astral_lock);

		if (rearm)
			mod_delayed_work(system_wq, &astral_probe_work,
					 msecs_to_jiffies(ASTRAL_RETRY_DELAY_MS));
		break;
	case BUS_NOTIFY_DEL_DEVICE:
		mutex_lock(&astral_lock);
		if (astral_client && &astral_client->dev == dev) {
			dev_info(dev, "astral-hwmon: chip removed, releasing it\n");
			astral_client = NULL;
		}
		mutex_unlock(&astral_lock);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static bool astral_attached(void)
{
	bool attached;

	mutex_lock(&astral_lock);
	attached = astral_client != NULL;
	mutex_unlock(&astral_lock);

	return attached;
}

/*
 * Stages 2 and 3: wait for the nvidia driver to publish the card's i2c
 * adapters, then wait for the chip on adapter 1 to answer. Which of the two we
 * are waiting on is reported, so a machine that never attaches says why.
 */
static void astral_probe_worker(struct work_struct *work)
{
	struct astral_scan scan = {};
	bool report;
	int attempts;

	astral_probe_once(&scan);

	if (astral_attached())
		return;

	mutex_lock(&astral_lock);
	attempts = astral_attempts;
	report = !astral_stage_reported;
	astral_stage_reported = true;
	mutex_unlock(&astral_lock);

	if (attempts < ASTRAL_RETRY_LIMIT) {
		/* One line the first time each stage is entered, then silence. */
		if (report) {
			if (!scan.card_adapters)
				pr_info("astral-hwmon: %s present; waiting for the NVIDIA driver to publish its i2c adapters\n",
					astral_model);
			else if (!scan.candidates)
				pr_info("astral-hwmon: %s has %d i2c adapters but none is '%s %d'; waiting\n",
					astral_model, scan.card_adapters,
					"NVIDIA i2c adapter", ASTRAL_ADAPTER_INDEX);
		}
		schedule_delayed_work(&astral_probe_work,
				      msecs_to_jiffies(ASTRAL_RETRY_DELAY_MS));
		return;
	}

	/* Out of patience. Say which stage we got stuck at - that is the whole
	 * point of tracking them separately.
	 */
	if (!scan.card_adapters)
		pr_warn("astral-hwmon: %s is present but the NVIDIA driver published no i2c adapters within %d s; is the proprietary driver loaded?\n",
			astral_model,
			ASTRAL_RETRY_LIMIT * ASTRAL_RETRY_DELAY_MS / 1000);
	else if (!scan.candidates)
		pr_warn("astral-hwmon: %s exposes %d i2c adapters but no '%s %d'; cannot locate the sensor bus\n",
			astral_model, scan.card_adapters,
			"NVIDIA i2c adapter", ASTRAL_ADAPTER_INDEX);
	else
		pr_warn("astral-hwmon: %s: the chip at 0x%02x did not answer within %d s; load with allow_unknown=1 or report this card\n",
			astral_model, addr,
			ASTRAL_RETRY_LIMIT * ASTRAL_RETRY_DELAY_MS / 1000);
}

static int __init astral_init(void)
{
	struct astral_scan scan = {};
	int ret;

	/* Catch a typo here rather than after 30 s of retries reporting that
	 * "the chip did not answer", which points the user at their card when
	 * the problem is their command line. 0x00-0x07 and 0x78-0x7f are
	 * reserved by the I2C specification and are not addressable.
	 */
	if (addr < 0x08 || addr > 0x77) {
		pr_err("astral-hwmon: addr=0x%x is not a valid 7-bit I2C address (0x08-0x77)\n",
		       addr);
		return -EINVAL;
	}

	/* Stage 1: refuse to stay resident on a machine that has no supported
	 * card. Answered from PCI, so it does not depend on the nvidia driver
	 * having loaded yet - which is exactly what makes it a usable gate.
	 */
	if (!astral_card_present(allow_unknown, &astral_model)) {
		pr_info("astral-hwmon: no supported ASUS ROG Astral card in this system; not loading\n");
		return -ENODEV;
	}

	pr_info("astral-hwmon: %s detected\n", astral_model);

	astral_nb.notifier_call = astral_bus_notify;
	ret = bus_register_notifier(&i2c_bus_type, &astral_nb);
	if (ret)
		return ret;

	/* Synchronous first pass, deliberately: `modprobe astral-hwmon && sensors`
	 * must work, so the sensors have to exist by the time modprobe returns
	 * whenever the card is already up. Only when it is absent or not yet
	 * ready - the boot case, where the nvidia adapters appear later - do we
	 * fall back to the retry worker.
	 */
	astral_probe_once(&scan);
	if (!astral_attached())
		schedule_delayed_work(&astral_probe_work,
				      msecs_to_jiffies(ASTRAL_RETRY_DELAY_MS));

	return 0;
}

static void __exit astral_exit(void)
{
	/* Notifier first. That is what makes the i2c_unregister_device() below
	 * safe to call with astral_lock held: its DEL notification can no
	 * longer re-enter astral_bus_notify() and deadlock on the same lock.
	 */
	bus_unregister_notifier(&i2c_bus_type, &astral_nb);
	/* Before taking the lock: the worker takes it, and cancel waits. */
	cancel_delayed_work_sync(&astral_probe_work);

	mutex_lock(&astral_lock);
	if (astral_client) {
		/* devm tears down hwmon, the data and the mutex with the client. */
		i2c_unregister_device(astral_client);
		astral_client = NULL;
	}
	mutex_unlock(&astral_lock);
}

module_init(astral_init);
module_exit(astral_exit);

MODULE_AUTHOR("Krzysztof Sokołowski");
MODULE_DESCRIPTION("Per-pin 12VHPWR monitoring for ASUS ROG Astral cards");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.2.1");
