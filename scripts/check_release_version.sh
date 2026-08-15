#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Krzysztof Sokołowski
#
# Release gate: the pushed tag must match the version in every place that
# carries one, so a forgotten bump fails loudly instead of shipping a module
# whose modinfo contradicts the release name.
#
# dkms.conf is authoritative - the Makefile derives DKMS_VERSION from it, and
# astral-guard bakes that in as GUARD_VERSION. The other two are maintained by
# hand, which is exactly why they need checking.
#   usage: scripts/check_release_version.sh v0.2.1
# A -suffix (rc/beta) is allowed on the tag and ignored for the comparison.
set -eu
cd "$(dirname "$0")/.."
tag="${1:?usage: check_release_version.sh <tag>}"

case "$tag" in
    v[0-9]*) ;;
    *) echo "check_release_version: tag '$tag' must look like v1.2.3" >&2; exit 1 ;;
esac
base="${tag#v}"; base="${base%%-*}"

dkms=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
module=$(sed -n 's/^MODULE_VERSION("\(.*\)");/\1/p' driver/astral_hwmon.c)
py=$(sed -n 's/^version = "\(.*\)"/\1/p' pyproject.toml | head -1)

[ -n "$dkms" ]   || { echo "check_release_version: no PACKAGE_VERSION in dkms.conf" >&2; exit 2; }
[ -n "$module" ] || { echo "check_release_version: no MODULE_VERSION in driver/astral_hwmon.c" >&2; exit 2; }
[ -n "$py" ]     || { echo "check_release_version: no version in pyproject.toml" >&2; exit 2; }

fail=0
for pair in "dkms.conf:$dkms" "MODULE_VERSION:$module" "pyproject.toml:$py"; do
    name=${pair%%:*}; val=${pair#*:}
    if [ "$val" != "$base" ]; then
        echo "check_release_version: $name is $val, tag $tag says $base" >&2
        fail=1
    fi
done
[ "$fail" -eq 0 ] || exit 1

echo "check_release_version: OK - $tag matches dkms.conf, MODULE_VERSION and pyproject.toml"
