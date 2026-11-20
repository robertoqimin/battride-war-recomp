#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d /tmp/battride-timebase.XXXXXX)"
trap 'rm -rf "$work"' EXIT
"${CC:-clang}" -std=c17 -O2 -pthread "$root/port/core/timebase.c" "$root/tests/test_timebase.c" -lm -o "$work/test"
"$work/test"
PORT_TIMEBASE=os "$work/test"
