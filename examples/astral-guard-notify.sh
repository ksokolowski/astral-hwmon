#!/bin/sh
# Example only. Not installed.
#
# Puts a desktop notification in front of anyone logged in graphically, for
# the case astral-guard-poweroff.service cannot otherwise reach: a user who
# is full-screen in a game or a browser with no terminal open, where wall(1)
# lands somewhere nobody is looking.
#
#   cp astral-guard-notify.sh /usr/local/bin/astral-guard-notify
#   chmod +x /usr/local/bin/astral-guard-notify
#
# then uncomment the matching ExecStartPre= in astral-guard-poweroff.service.
#
# WHY A SCRIPT AND NOT AN ExecStartPre= ONE-LINER. Two reasons, both found by
# trying it. systemd expands ${...} in a command line itself, so any shell
# parameter expansion is replaced with the empty string before /bin/sh ever
# sees it. And /run is mounted noexec on at least Ubuntu, so this cannot be
# generated into /run at runtime either.
#
# It speaks org.freedesktop.Notifications, which GNOME, KDE Plasma, XFCE,
# Cinnamon, MATE and Budgie all implement, on Wayland and X11 alike - it is a
# D-Bus call, not a display-server one. It needs notify-send, which is
# libnotify-bin on Debian and Ubuntu. If that is missing this exits non-zero,
# which is why the unit calls it with a leading '-': failing to warn must
# never stop the machine from protecting itself.
#
# It reaches nobody on a headless box. That is the same machine where the
# poweroff matters most, so this supplements wall(1) rather than replacing it.

secs=${1:-60}
rc=1

for d in /run/user/*; do
    # A session bus is the only reliable sign of a graphical login. Users
    # listed by loginctl without one cannot be notified.
    [ -S "$d/bus" ] || continue
    uid=${d##*/}
    # runuser wants a name; "#1000" is a sudo convention and fails here.
    user=$(getent passwd "$uid" | cut -d: -f1)
    [ -n "$user" ] || continue

    # -u critical is the point: GNOME and KDE keep critical notifications on
    # screen instead of fading them, and show them through Do Not Disturb.
    runuser -u "$user" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$d/bus" \
        notify-send -u critical -i dialog-warning \
        "12VHPWR CRITICAL" \
        "astral-guard reports a critical connector fault. Powering off in ${secs}s. Cancel: sudo systemctl stop astral-guard-poweroff.service" \
        && rc=0
done

exit "$rc"
