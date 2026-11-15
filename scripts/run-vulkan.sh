#!/usr/bin/env bash
set -euo pipefail
port_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$port_root"
export PS3_VFS_ROOT="$port_root/game"
# Codecs extracted from this disc's PS3_UPDATE firmware, kept port-local.
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-$port_root/firmware/dev_flash}"
export PS3RECOMP_VK_GUEST_PROGRAMS="${PS3RECOMP_VK_GUEST_PROGRAMS:-1}"
export PS3RECOMP_VK_WINDOW="${PS3RECOMP_VK_WINDOW:-1}"
export PORT_GCM_EXPAND_BUFFERS="${PORT_GCM_EXPAND_BUFFERS:-0}"
ulimit -c 0
ulimit -f 262144
exec timeout --kill-after=2s "${PORT_TEST_SECONDS:-0}s" "$port_root/build-vulkan/kamenriderbattridewarsport" "$port_root/game/EBOOT.ELF" "$@"
