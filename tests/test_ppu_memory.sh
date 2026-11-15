#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${PS3RECOMP_DIR:-$root/../ps3recomp}"
work="$(mktemp -d /tmp/battride-memory.XXXXXX)"
trap 'rm -rf "$work"' EXIT
includes=(-I"$root/include" -I"$root/src/recomp" -I"$sdk/include" -I"$sdk/runtime/spu")
checks=()
if [[ "${SANITIZE:-0}" == 1 ]]; then
    checks+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-clang}" -std=c17 -O2 "${includes[@]}" "${checks[@]}" \
    "-DPORT_SPU_COHERENCY_SOURCE=\"$sdk/runtime/spu/spu_coherency.c\"" \
    -c "$root/port/spu/spu_coherency_bridge.c" -o "$work/coherency.o"
"${CC:-clang}" -std=c17 -O2 "${checks[@]}" -c "$root/port/core/store_wrap.c" -o "$work/store_wrap.o"
"${CXX:-g++}" -std=c++20 -O2 -fno-strict-aliasing "${includes[@]}" "${checks[@]}" \
    "$root/tests/test_ppu_memory.cpp" "$work/coherency.o" "$work/store_wrap.o" -o "$work/test"
ASAN_OPTIONS=detect_leaks=0 "$work/test"
