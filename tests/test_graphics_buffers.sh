#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${PS3RECOMP_DIR:-$root/../ps3recomp}"
binary="$(mktemp /tmp/battride-graphics-test.XXXXXX)"
trap 'rm -f "$binary"' EXIT
"${CXX:-g++}" -std=c++20 -O0 -ffunction-sections -fdata-sections -I"$root/src/recomp" -I"$sdk/include" "$root/port/core/stubs.cpp" "$root/tests/test_graphics_buffers.cpp" -Wl,--gc-sections -o "$binary"
"$binary"
