#!/usr/bin/env bash
set -euo pipefail
port_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
port_test_dir="$(mktemp -d "${TMPDIR:-/tmp}/port-sail-test.XXXXXX")"
trap 'rm -rf -- "$port_test_dir"' EXIT
mkdir -p "$port_test_dir/PS3_GAME"
ffmpeg -nostdin -v error -f lavfi -i testsrc2=size=64x48:rate=30 \
    -frames:v 6 -c:v mpeg2video -f mpeg "$port_test_dir/PS3_GAME/test.pam"
port_flags=()
port_compiler="${CXX:-c++}"
if [[ "${SANITIZE:-0}" == 1 ]]; then
    port_compiler="${CXX:-clang++}"
    port_flags=(-fsanitize=address,undefined)
    # LeakSanitizer cannot inspect threads under this app's sandbox/ptrace.
    export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
fi
"$port_compiler" -std=c++20 -O1 -g -pthread "${port_flags[@]}" \
    -I "$port_root/port/audio" -I "$port_root" -I "$port_root/src/recomp" \
    "$port_root/tests/test_sail.cpp" -lstdc++ \
    "$port_root/build-vulkan/ps3recomp_sdk/libps3recomp_runtime.a" -o "$port_test_dir/test_sail"
"$port_test_dir/test_sail" "$port_test_dir"
