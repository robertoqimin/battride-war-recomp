#!/usr/bin/env bash
set -euo pipefail
port_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
port_test_dir="$(mktemp -d "${TMPDIR:-/tmp}/port-sail-audio.XXXXXX")"
trap 'rm -rf -- "$port_test_dir"' EXIT
port_movie="${1:-$port_root/game/PS3_GAME/USRDIR/nativePS/movie/TAISEN_130220.pam}"
port_flags=()
port_compiler="${CXX:-c++}"
if [[ "${SANITIZE:-0}" == 1 ]]; then
    port_compiler="${CXX:-clang++}"
    port_flags=(-fsanitize=address,undefined)
    export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
fi
"$port_compiler" -std=c++20 -O1 -g -pthread "${port_flags[@]}" \
    -I "$port_root/port/audio" "$port_root/tests/test_sail_audio.cpp" -lstdc++ \
    "$port_root/build-vulkan/ps3recomp_sdk/libps3recomp_runtime.a" -o "$port_test_dir/test_sail_audio"
"$port_test_dir/test_sail_audio" "$port_movie" "${2:-6533120}"
