#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${PS3RECOMP_DIR:-$root/../ps3recomp}"
dir="$(mktemp -d /tmp/battride-remap.XXXXXX)"
trap 'rm -rf "$dir"' EXIT
cc="${CC:-clang}"
flags=(-std=c17 -O2 -ffp-contract=off -ffunction-sections -fdata-sections -I"$sdk/include" -I"$sdk/libs/video")
"$cc" "${flags[@]}" -Drsx_vertex_remap_build=reference_remap -c "$sdk/libs/video/rsx_vertex_compact.c" -o "$dir/ref.o"
objcopy --keep-global-symbol=reference_remap "$dir/ref.o"
"$cc" "${flags[@]}" -march=native -c "$root/port/graphics/rsx_vertex_bridge.c" -o "$dir/port.o"
"$cc" "${flags[@]}" "$root/tests/test_vertex_remap.c" "$dir/ref.o" "$dir/port.o" -Wl,--gc-sections -o "$dir/test"
"$dir/test"
