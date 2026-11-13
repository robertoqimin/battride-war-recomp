#!/usr/bin/env python3
"""Train GCC profiles for a PORT_PGO=generate build by playing the opening mission.

Each session boots the game, gets through the menus and the tutorial with
scripted presses, fights for a minute in STG02 and ends with SIGTERM, which
makes the training build write its profiles (see main.cpp). Counters add up
across sessions. Afterwards profile.json records what the profiles match: the
compiler, the SDK commit and the EBOOT. The profiles are execution counts of
the translated game code: keep them private like the generated sources.

usage: pgo_train.py --build <generate build dir> [--sessions 2]
The game, firmware and save data are taken from the repository's game/,
firmware/ and gamedata/ folders, as for run-vulkan.sh.

On Windows, run it from the MSYS2 UCRT64 shell with ffmpeg on PATH. A session
ends with Ctrl+Break there, which the training build takes as SIGTERM.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
COMBAT_DRAWS = 550       # draws per frame once the first fight is on screen
WINDOWS = os.name == 'nt'
# The game in a process group of its own: Ctrl+Break reaches only that group.
NEW_GROUP = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {'start_new_session': True}


def kill_group(proc):
    if WINDOWS:
        proc.kill()
    else:
        os.killpg(proc.pid, signal.SIGKILL)


def stop_group(proc):
    """What makes a training build write its profiles and exit (main.cpp)."""
    if WINDOWS:
        os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
    else:
        os.killpg(proc.pid, signal.SIGTERM)


def cache_value(build, name, *fallbacks):
    lines = (build / 'CMakeCache.txt').read_text().splitlines()
    for key in (name, *fallbacks):
        for line in lines:
            if line.startswith(key + ':'):
                return line.split('=', 1)[1]
    raise SystemExit('%s not in %s/CMakeCache.txt' % (name, build))


def session(binary, work, seconds):
    shutil.copytree(ROOT / 'gamedata', work / 'gamedata')
    pad = work / 'pad.txt'
    pad.touch()
    env = {k: v for k, v in os.environ.items() if k not in ('PAD_SCRIPT', 'PAD_AUTOPRESS', 'LD_LIBRARY_PATH')}
    env.update(PS3_GAMEDATA_ROOT=str(work / 'gamedata'), PS3_VFS_ROOT=str(ROOT / 'game'),
               PS3_DEV_FLASH=str(ROOT / 'firmware/dev_flash'), PS3RECOMP_VK_WINDOW='1',
               PS3RECOMP_VK_GUEST_PROGRAMS='1', PAD_FILE=str(pad), PAD_STICK='128,128,128,128',
               PAD_NO_KEYBOARD='1', PAD_ALWAYS_CHANGE='1', PORT_GCM_FRAME_STATS='30')
    log_path = work / 'run.log'
    with log_path.open('w') as log:
        proc = subprocess.Popen([str(binary), str(ROOT / 'game/EBOOT.ELF')], cwd=ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT, **NEW_GROUP)
        trained = False
        try:
            # The instrumented build runs at a few frames per second: be patient.
            start, next_press = time.monotonic(), time.monotonic() + 15
            while True:
                if proc.poll() is not None or time.monotonic() - start > 900:
                    raise SystemExit('Combat not reached; see ' + str(log_path))
                draws = re.findall(r'\[fstat\] flips=30.*?draws=(\d+)/frame', log_path.read_text(errors='replace'))
                if draws and int(draws[-1]) > COMBAT_DRAWS:
                    break
                if time.monotonic() >= next_press:      # CIRCLE: confirm menus and dialogue
                    pad.write_text('0x2000 60\n')
                    next_press = time.monotonic() + 7
                time.sleep(.5)
            # START dismisses the tutorial prompt that pauses the first fight.
            # It can appear after the draw count passes the threshold, so press
            # until the scene moves (a paused frame repeats its draw count);
            # otherwise the profiles would come from the paused frame.
            for attempt in range(10):
                pad.write_text('0x0008 40\n')
                time.sleep(4)
                recent = re.findall(r'\[fstat\] flips=30.*?draws=(\d+)/frame',
                                    log_path.read_text(errors='replace'))[-5:]
                if len(set(recent)) > 1:
                    break
            else:
                raise SystemExit('The first fight stayed paused; see ' + str(log_path))
            end, next_press = time.monotonic() + seconds, 0
            next_check, checked_at, unfreezes = time.monotonic() + 1, 0, 0
            while time.monotonic() < end:
                if proc.poll() is not None:
                    raise SystemExit('The game exited during training; see ' + str(log_path))
                # A later tutorial prompt pauses the fight again, and SQUARE
                # does not close it: the profiles then came mostly from the
                # paused screen. Same rule as bench_combat.py: four [fstat]
                # lines with one draw count are a pause; START closes a prompt,
                # CIRCLE a dialogue box.
                if time.monotonic() >= next_check:
                    next_check = time.monotonic() + 1.0
                    recent = re.findall(r'\[fstat\] flips=30.*?draws=(\d+)/frame',
                                        log_path.read_text(errors='replace'))
                    if len(recent) >= checked_at + 4 and len(set(recent[-4:])) == 1:
                        pad.write_text('0x0008 40\n' if unfreezes % 2 == 0 else '0x2000 40\n')
                        unfreezes += 1
                        checked_at = len(recent)
                        next_press = time.monotonic() + 1.5
                if time.monotonic() >= next_press:      # SQUARE: attack
                    pad.write_text('0x8000 18\n')
                    next_press = time.monotonic() + 1.5
                time.sleep(.05)
            trained = True
        finally:
            if proc.poll() is None and not trained:
                # A session that never reached the fight must not add its
                # menu counts to the profiles: SIGKILL writes nothing.
                kill_group(proc)
                proc.wait()
            elif proc.poll() is None:
                stop_group(proc)                         # writes the profiles
                try:
                    proc.wait(timeout=300)
                except subprocess.TimeoutExpired:
                    kill_group(proc)
                    proc.wait()
                    raise SystemExit('The training build did not exit after SIGTERM; profiles may be incomplete.')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', type=Path, required=True, help='build directory configured with PORT_PGO=generate')
    ap.add_argument('--sessions', type=int, default=2)
    ap.add_argument('--seconds', type=float, default=60, help='combat time per session')
    args = ap.parse_args()
    build = args.build.resolve()
    if cache_value(build, 'PORT_PGO') != 'generate':
        raise SystemExit('Configure the build with -DPORT_PGO=generate first.')
    profiles = Path(cache_value(build, 'PORT_PGO_DIR'))
    binary = build / ('kamenriderbattridewarsport.exe' if WINDOWS else 'kamenriderbattridewarsport')
    spec = json.loads((ROOT / 'recompile.json').read_text())
    elf = hashlib.sha256((ROOT / 'game/EBOOT.ELF').read_bytes()).hexdigest()
    if elf != spec['eboot_sha256']:
        raise SystemExit('game/EBOOT.ELF is not the decrypted BLJS10220 EBOOT this port expects.')
    if profiles.exists() and any(profiles.rglob('*.gcda')):
        raise SystemExit('Profiles already exist in %s; move them away to start over.' % profiles)
    for n in range(args.sessions):
        print('Training session %d of %d...' % (n + 1, args.sessions), flush=True)
        # The scripted menu route sometimes stalls before the fight: start the
        # session again.
        for attempt in range(3):
            with tempfile.TemporaryDirectory(prefix='pgo-train-') as work:
                try:
                    session(binary, Path(work), args.seconds)
                    break
                except SystemExit as e:
                    if attempt == 2 or not ('Combat not reached' in str(e) or 'stayed paused' in str(e)):
                        raise
                    print('  %s; retrying (that attempt wrote no profiles)' % e, flush=True)
    record(build, profiles, spec, elf, args.sessions)


def record(build, profiles, spec, elf, sessions):
    files = sorted(str(p.relative_to(profiles)) for p in profiles.rglob('*.gcda'))
    if not files:
        raise SystemExit('No profiles were written.')
    # A toolchain file sets the compiler without caching it; CMake records it here.
    found = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)',
                      next(build.glob('CMakeFiles/*/CMakeCXXCompiler.cmake')).read_text())
    version = subprocess.check_output([found[1], '-dumpfullversion'], text=True).strip()
    (profiles / 'profile.json').write_text(json.dumps({
        'gcc': version, 'sdk_commit': spec['sdk_commit'], 'eboot_sha256': elf,
        'sessions': sessions, 'files': len(files),
        # SDK sources outside the build tree keep their absolute path in the
        # profile names; the importer renames those for the player's SDK copy.
        # Without -DPS3RECOMP_DIR (the windows-vulkan preset) CMake uses ../ps3recomp
        # uncached; the SDK subproject records the same directory.
        'sdk_dir': cache_value(build, 'PS3RECOMP_DIR', 'ps3recomp_runtime_SOURCE_DIR')}, indent=2) + '\n')
    print('Wrote %d profiles for GCC %s to %s' % (len(files), version, profiles))


if __name__ == '__main__':
    main()
