# Makefile - the one command interface. Prefer these over raw tools.
.DEFAULT_GOAL := help
UV := uv
PYTHONPATH := src

.PHONY: help setup test test-hw coverage lint lint-fix format format-check burn \
        type-check qa clean hooks install-hooks guard install-guard

help:
	@grep -E '^[a-z][a-z-]*:.*?## .*$$' $(MAKEFILE_LIST) | \
	  awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-16s\033[0m %s\n", $$1, $$2}'

setup: ## Install dev dependencies and the git hooks
	$(UV) sync --all-extras
	$(MAKE) install-hooks

install-hooks: ## Point git at the repo's own hooks (sole-human-author gate)
	git config core.hooksPath scripts/hooks
	chmod +x scripts/hooks/*

test: ## Unit tests only - no hardware needed, safe anywhere
	PYTHONPATH=$(PYTHONPATH) $(UV) run pytest tests/unit

test-hw: ## Hardware tests - needs a supported Astral card; skips otherwise
	PYTHONPATH=$(PYTHONPATH) $(UV) run pytest tests/hardware -m hardware

# The native tier compiles the driver's own pure sources for the host against
# the kernel shims in tests/native/linux, so the code that ships is checked
# against the same corpus the Python mirror is. No kernel headers, no root, no
# card - it runs anywhere gcc does.
NATIVE_BIN := tests/native/astral-native-tests
# -Wno-unused-parameter mirrors driver/Kbuild. The native tier must not be
# stricter than the module build, or driver code that compiles as a module
# fails only here.
NATIVE_CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -Werror \
                 -fsanitize=address,undefined -fno-omit-frame-pointer \
                 -Itests/native -Itests/support -Idriver -Iguard
# astral_detect.c is #included by test_detect.c to reach its statics, so it must
# not be compiled again here; astral_regs.c has no statics and is linked plainly.
NATIVE_SRCS := tests/native/main.c tests/native/harness.c \
               tests/native/test_regs.c tests/native/test_detect.c \
               tests/native/test_names.c \
               tests/support/check.c \
               driver/astral_regs.c
NATIVE_DEPS := $(NATIVE_SRCS) driver/astral_detect.c driver/astral.h \
               tests/native/harness.h tests/support/check.h \
               guard/astral_guard.h \
               $(wildcard tests/native/linux/*.h)

.PHONY: test-native

$(NATIVE_BIN): $(NATIVE_DEPS)
	$(CC) $(NATIVE_CFLAGS) -o $@ $(NATIVE_SRCS)

test-native: $(NATIVE_BIN) ## Native C tests - the shipped decoder and PCI gate, no hardware
	./$(NATIVE_BIN) tests/data/frames.jsonl

# The guard is userspace C built against the real system headers, so it must not
# see tests/native/linux/. Separate binary, separate flags.
GUARD_BIN := tests/guard/astral-guard-tests
# -std=c11 implies __STRICT_ANSI__, which hides the POSIX declarations. The
# feature-test macro asks for exactly the standard the guard is allowed to use:
# POSIX.1-2008, no GNU extensions, so it builds on glibc and musl alike.
GUARD_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L \
                -g -O1 -Wall -Wextra -Werror \
                -fsanitize=address,undefined -fno-omit-frame-pointer \
                -Iguard -Itests/support -Itests/guard
GUARD_LIB_SRCS := guard/guard_eval.c guard/guard_sysfs.c \
                  guard/guard_report.c
GUARD_TEST_SRCS := tests/guard/main.c tests/guard/test_eval.c \
                   tests/guard/test_sysfs.c tests/guard/fakesys.c \
                   tests/guard/test_cli.c \
                   tests/guard/corpus.c tests/support/check.c
GUARD_DEPS := $(GUARD_LIB_SRCS) $(GUARD_TEST_SRCS) guard/astral_guard.h \
              tests/guard/corpus.h tests/guard/fakesys.h \
              tests/support/check.h

.PHONY: test-guard

$(GUARD_BIN): $(GUARD_DEPS)
	$(CC) $(GUARD_CFLAGS) -o $@ $(GUARD_LIB_SRCS) $(GUARD_TEST_SRCS)

test-guard: $(GUARD_BIN) ## Guard tier - the shipped rule engine, no hardware
	./$(GUARD_BIN) tests/data/guard-cases.jsonl

coverage: ## Unit tests with coverage
	PYTHONPATH=$(PYTHONPATH) $(UV) run pytest tests/unit \
	  --cov=astral_oracle --cov-report=term-missing

lint: ## Ruff check
	$(UV) run ruff check src tests

lint-fix: ## Ruff check --fix
	$(UV) run ruff check --fix src tests

format: ## Ruff format
	$(UV) run ruff format src tests

format-check: ## Ruff format --check
	$(UV) run ruff format --check src tests

type-check: ## Mypy
	$(UV) run mypy src

# `guard` comes before `coverage` on purpose: tests/unit/test_guard_cli.py runs
# the built binary and skips without it, so building it here is what stops that
# test from silently never running in CI. `make test` still needs no compiler.
qa: lint format-check type-check guard coverage test-native test-guard ## Full gate

hooks: ## Run pre-commit on staged files
	$(UV) run pre-commit run

clean: ## Remove build and cache artefacts
	rm -rf .pytest_cache .ruff_cache .mypy_cache htmlcov .coverage
	rm -f tests/native/astral-native-tests tests/guard/astral-guard-tests
	rm -f guard/astral-guard
	find . -name __pycache__ -type d -prune -exec rm -rf {} +

burn: tools/burn ## Build the CUDA load generator

tools/burn: tools/burn.cu
	nvcc -O3 -o $@ $<

KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE := driver/astral-hwmon.ko

.PHONY: module module-clean module-load module-unload

module: ## Build the kernel module against the running kernel
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver modules

module-clean: ## Clean kernel build artefacts
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver clean

module-load: module ## Insert the freshly built module
	sudo insmod $(MODULE)

module-unload: ## Remove the module
	sudo rmmod astral_hwmon || true

DKMS_NAME := astral-hwmon
# dkms.conf is the single source of truth for the packaged version. Deriving it
# here stops the Makefile and dkms.conf drifting apart, which makes `dkms add`
# fail on a PACKAGE_VERSION mismatch.
DKMS_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

# The shipped binary. Version comes from dkms.conf via DKMS_VERSION, so this
# does not become a fourth place a version string is maintained by hand.
GUARD_PROG := guard/astral-guard
GUARD_PROG_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra \
                     -Werror -Iguard -DGUARD_VERSION='"$(DKMS_VERSION)"'
GUARD_PROG_SRCS := guard/guard_main.c guard/guard_eval.c \
                   guard/guard_sysfs.c guard/guard_report.c

# dkms.conf is a prerequisite because the version is baked in with -D: without
# it, bumping the version leaves a binary that still reports the old one.
$(GUARD_PROG): $(GUARD_PROG_SRCS) guard/astral_guard.h dkms.conf
	$(CC) $(GUARD_PROG_CFLAGS) -o $@ $(GUARD_PROG_SRCS)

guard: $(GUARD_PROG) ## Build the astral-guard binary

# DESTDIR and PREFIX are the entire packaging interface, which is what every
# distro expects and why no autotools or CMake is needed here. Nothing lands in
# /etc and nothing is enabled: the integration snippets are documentation, so
# the tool keeps making no init-system assumption.
PREFIX ?= /usr/local

install-guard: $(GUARD_PROG) ## Install astral-guard (honours DESTDIR and PREFIX)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(GUARD_PROG) $(DESTDIR)$(PREFIX)/bin/astral-guard
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 guard/astral-guard.1 \
	  $(DESTDIR)$(PREFIX)/share/man/man1/astral-guard.1
	install -d $(DESTDIR)$(PREFIX)/share/doc/astral-hwmon/examples
	install -m 644 examples/* \
	  $(DESTDIR)$(PREFIX)/share/doc/astral-hwmon/examples/

# %.mod.c is generated by Kbuild; shipping it into /usr/src breaks the build there.
DRIVER_SRCS := $(filter-out %.mod.c,$(wildcard driver/*.c))

.PHONY: dkms-install dkms-remove

dkms-install: dkms-remove ## Install via DKMS so it survives kernel upgrades (re-runnable)
	sudo install -d $(DKMS_SRC)/driver
	sudo install -m 644 driver/Kbuild driver/astral.h $(DRIVER_SRCS) $(DKMS_SRC)/driver/
	sudo install -m 644 dkms.conf $(DKMS_SRC)/
	sudo dkms add -m $(DKMS_NAME) -v $(DKMS_VERSION)
	sudo dkms build -m $(DKMS_NAME) -v $(DKMS_VERSION)
	sudo dkms install -m $(DKMS_NAME) -v $(DKMS_VERSION)

# Removes *every* installed version, not just the current one. Targeting only
# DKMS_VERSION meant a version bump left the previous release resident and its
# tree in /usr/src, so `dkms status` listed two and the older .ko.zst stayed in
# updates/dkms - the same stale-duplicate trap as hand-copying a .ko.
dkms-remove: ## Remove every installed DKMS version (no-op if absent)
	-@for v in $$(dkms status -m $(DKMS_NAME) 2>/dev/null | \
	              sed -n 's|^$(DKMS_NAME)/\([^,]*\),.*|\1|p' | sort -u); do \
	    echo "removing $(DKMS_NAME)/$$v"; \
	    sudo dkms remove -m $(DKMS_NAME) -v $$v --all 2>/dev/null || true; \
	    sudo rm -rf /usr/src/$(DKMS_NAME)-$$v; \
	  done
	sudo rm -rf $(DKMS_SRC)
	# Legacy: autoload is by PCI modalias now, not a hardcoded module list.
	sudo rm -f /etc/modules-load.d/astral-hwmon.conf
