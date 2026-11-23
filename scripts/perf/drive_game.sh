#!/usr/bin/env bash
# Drive the game by hand from a script: start it with a save, press buttons,
# take screenshots. Used to reach stages the benchmark route does not
# (docs/PLANO0.6.1.md, N1/N2).
#
#   drive_game.sh start [save folder]   start this checkout's build (integrated GPU)
#   drive_game.sh press <mask> [polls]  hold a button: 0x2000 O, 0x4000 X, 0x8000 square,
#                                       0x1000 triangle, 0x0008 start, 0x0010 up,
#                                       0x0040 down, 0x0080 left, 0x0020 right
#   drive_game.sh shot <name>           the window's next frame as <work>/<name>.png
#   drive_game.sh log <regex> [n]       the last n matching lines of the game log
#   drive_game.sh stop
#
# The work folder is $DRIVE_DIR (default /tmp/battride-drive): game.log, the
# screenshots and a copy of the save, so the original is never written.
# Extra environment for the game goes in $DRIVE_ENV ("NAME=value ...").
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
work="${DRIVE_DIR:-/tmp/battride-drive}"
case "${1:-}" in
start)
    rm -rf "$work"; mkdir -p "$work/gamedata/dev_hdd0/home/00000001/savedata"
    if [ -n "${2:-}" ]; then cp -r "$2" "$work/gamedata/dev_hdd0/home/00000001/savedata/"; fi
    : > "$work/pad"
    cd "$root"
    # shellcheck disable=SC2086
    env -u LD_LIBRARY_PATH PS3RECOMP_VK_DEVICE="${PS3RECOMP_VK_DEVICE:-0}" PS3_GAMEDATA_ROOT="$work/gamedata" \
        PS3_VFS_ROOT="$root/game" PS3_DEV_FLASH="$root/firmware/dev_flash" PS3RECOMP_VK_WINDOW=1 \
        PS3RECOMP_VK_GUEST_PROGRAMS=1 PORT_GCM_EXPAND_BUFFERS=0 PAD_FILE="$work/pad" PAD_NO_KEYBOARD=1 \
        PORT_WINDOW_DUMP="$work/shot.ppm" PORT_WINDOW_DUMP_TRIGGER="$work/trigger" ${DRIVE_ENV:-} \
        nohup build-vulkan/kamenriderbattridewarsport game/EBOOT.ELF > "$work/game.log" 2>&1 &
    echo $! > "$work/pid"; echo "started $(cat "$work/pid"), log $work/game.log";;
press) echo "$2 ${3:-6}" > "$work/pad"; sleep 0.4;;
shot)
    rm -f "$work/shot.ppm"; touch "$work/trigger"
    for _ in $(seq 50); do [ -s "$work/shot.ppm" ] && [ ! -e "$work/trigger" ] && break; sleep 0.1; done
    sleep 0.3
    ffmpeg -loglevel error -y -i "$work/shot.ppm" -vf scale=640:-1 "$work/$2.png" && echo "$work/$2.png";;
log) grep -E "$2" "$work/game.log" | tail -"${3:-10}" || true;;
stop) kill "$(cat "$work/pid")" 2>/dev/null || true; echo stopped;;
*) sed -n '2,20p' "$0"; exit 2;;
esac
