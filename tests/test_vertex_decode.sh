#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${PS3RECOMP_DIR:-$root/../ps3recomp}"
dir="$(mktemp -d /tmp/battride-vertex.XXXXXX)"
trap 'rm -rf "$dir"' EXIT
cc="${CC:-clang}"
flags=(-std=c17 -O2 -ffp-contract=off -ffunction-sections -fdata-sections -I"$sdk/include" -I"$sdk/libs/video")
"$cc" "${flags[@]}" -Drsx_vertex_decode_element=reference_decode -c "$sdk/libs/video/rsx_vertex_compact.c" -o "$dir/ref.o"
# Only retain the reference decoder when linking: other public helpers would
# collide with the port copy and are not part of this comparison.
objcopy --keep-global-symbol=reference_decode "$dir/ref.o"
"$cc" "${flags[@]}" -march=native -c "$root/port/graphics/rsx_vertex_bridge.c" -o "$dir/port.o"
"${CXX:-g++}" -std=c++20 -O2 -I"$sdk/include" -I"$sdk/libs/video" "$root/tests/test_vertex_decode.cpp" "$dir/ref.o" "$dir/port.o" -Wl,--gc-sections -o "$dir/test"
"$dir/test"
