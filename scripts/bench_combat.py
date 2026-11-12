#!/usr/bin/env python3
"""Benchmark heavy combat: the same scripted route as pgo_train.py, then measured fighting.

Each run boots the game, gets through the menus and the tutorial with scripted
presses, waits until the first fight is on screen (more than 550 draws a
frame), attacks for --seconds and records every guest frame (PORT_FRAME_TIMES)
and the CPU time of each thread. Runs are reported one by one and together:

  avg fps        frames / seconds over the measured window
  p95 / p99 ms   95th / 99th percentile frame interval
  >16.7 / >33 ms frames that missed 60 / 30 fps
  1% low         fps from the slowest 1% of frames
  worst 5 s      lowest fps over any 5-second span
  threads        busiest threads, % of one CPU, over the window

usage: bench_combat.py --binary <game executable> [--runs 3] [--seconds 60]
                       [--label name] [--json out.json] [-- extra ENV=VALUE ...]
The game, firmware and save data come from the repository's game/, firmware/
and gamedata/ folders; the save is copied, never written.

On Windows, run it from the MSYS2 UCRT64 shell (the game's DLLs) with ffmpeg on
PATH. There is no /proc there: no per-thread CPU, --pin or --stacks.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
COMBAT_DRAWS = 550
WINDOWS = os.name == 'nt'
CLK_TCK = 100 if WINDOWS else os.sysconf('SC_CLK_TCK')
# The game in a process group of its own, so the whole group can be stopped.
NEW_GROUP = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {'start_new_session': True}


def kill_group(proc):
    if WINDOWS:
        proc.kill()
    else:
        os.killpg(proc.pid, signal.SIGKILL)


def thread_times(pid):
    """{tid: (name, utime+stime ticks)} for every thread of pid."""
    out = {}
    if WINDOWS:
        return out

    for task in Path('/proc/%d/task' % pid).iterdir():
        try:
            stat = (task / 'stat').read_text()
            name = stat[stat.index('(') + 1:stat.rindex(')')]
            fields = stat[stat.rindex(')') + 2:].split()
            out[int(task.name)] = (name, int(fields[11]) + int(fields[12]))
        except (OSError, ValueError):
            pass
    return out


def run_once(binary, seconds, extra_env, work, stacks=0, pin=0, prefix=(), dump=None):
    shutil.copytree(ROOT / 'gamedata', work / 'gamedata')
    pad = work / 'pad.txt'
    pad.touch()
    frames = work / 'frames.csv'
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PAD_', 'PORT_')) and k != 'LD_LIBRARY_PATH'}
    env.update(PS3_GAMEDATA_ROOT=str(work / 'gamedata'), PS3_VFS_ROOT=str(ROOT / 'game'),
               PS3_DEV_FLASH=str(ROOT / 'firmware/dev_flash'), PS3RECOMP_VK_WINDOW='1',
               PS3RECOMP_VK_GUEST_PROGRAMS='1', PORT_GCM_EXPAND_BUFFERS='0', PAD_FILE=str(pad),
               PAD_STICK='128,128,128,128', PAD_NO_KEYBOARD='1', PAD_ALWAYS_CHANGE='1',
               PORT_GCM_FRAME_STATS='30', PORT_FRAME_TIMES=str(frames))
    env.update(extra_env)
    if dump:
        env.update(PORT_WINDOW_DUMP=str(dump), PORT_WINDOW_DUMP_TRIGGER=str(work / 'dump-now'))
    log_path = work / 'run.log'
    with log_path.open('w') as log:
        proc = subprocess.Popen([*prefix, str(binary), str(ROOT / 'game/EBOOT.ELF')], cwd=ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT, **NEW_GROUP)
        try:
            start, next_press = time.monotonic(), time.monotonic() + 15
            while True:
                if proc.poll() is not None or time.monotonic() - start > 600:
                    raise RuntimeError('combat not reached; see ' + str(log_path))
                draws = re.findall(r'\[fstat\] flips=30.*?draws=(\d+)/frame', log_path.read_text(errors='replace'))
                if draws and int(draws[-1]) > COMBAT_DRAWS:
                    break
                if time.monotonic() >= next_press:      # CIRCLE: confirm menus and dialogue
                    pad.write_text('0x2000 60\n')
                    next_press = time.monotonic() + 7
                time.sleep(.5)
            # START dismisses the tutorial prompt that pauses the first fight.
            # It can appear after the draw count first passes the threshold, so
            # press until the scene moves: a paused frame repeats the same draw
            # count in every [fstat] line.
            for attempt in range(10):
                pad.write_text('0x0008 40\n')
                time.sleep(4)
                recent = [int(d) for d in re.findall(r'\[fstat\] flips=30.*?draws=(\d+)/frame',
                                                     log_path.read_text(errors='replace'))[-5:]]
                if len(set(recent)) > 1:
                    break
            else:
                raise RuntimeError('the first fight stayed paused; see ' + str(log_path))
            if pin:
                pin_busiest(proc.pid, pin)
            t0_cpu, t0 = thread_times(proc.pid), time.monotonic()
            t0_ms = (t0 - start) * 1000.0
            print('combat measured from %.0f s after start' % (t0 - start), flush=True)
            next_press, samples, next_sample = 0, [], t0 + 5
            next_check, checked_at, unfreezes = t0 + 1, 0, 0
            dump_at = t0 + min(10.0, seconds / 2) if dump else None
            while time.monotonic() - t0 < seconds:
                # Diagnostics only: each eu-stack snapshot stops the game briefly.
                if stacks and len(samples) < stacks and time.monotonic() >= next_sample:
                    samples.append(subprocess.run(['eu-stack', '-p', str(proc.pid), '-n', '24'],
                                                  capture_output=True, text=True).stdout)
                    next_sample = time.monotonic() + max(1.0, (seconds - 10) / stacks)
                if proc.poll() is not None:
                    raise RuntimeError('the game exited during the benchmark; see ' + str(log_path))
                if dump_at and time.monotonic() >= dump_at:   # one frame of the fight to --dump
                    (work / 'dump-now').touch()
                    dump_at = None
                # Later tutorial prompts pause the fight too, 15-30 s into it,
                # and SQUARE does not close them: the window was then mostly a
                # still screen at the game's 60 fps cap. A pause shows as the
                # same draw count in four [fstat] lines in a row; START closes
                # a prompt (and the pause menu), CIRCLE a dialogue box.
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
            t1_cpu, wall = thread_times(proc.pid), time.monotonic() - t0
        finally:
            if proc.poll() is None:
                kill_group(proc)
                proc.wait()
    # Frame log times are from the game's own start; the window is the last
    # `seconds` of what it wrote before the kill.
    rows = [line.split(',') for line in frames.read_text().splitlines()[1:] if line.count(',') == 2]
    elapsed = [float(r[1]) for r in rows]
    end = elapsed[-1]
    window = [(int(r[0]), float(r[1]), float(r[2])) for r in rows if float(r[1]) >= end - seconds * 1000.0]
    # Each [fstat] line covers 30 guest frames, from the first. Frames in a
    # run of four or more identical draw counts were a paused screen.
    all_draws = [int(d) for d in re.findall(r'\[fstat\] flips=30.*?draws=(\d+)/frame', log_path.read_text(errors='replace'))]
    paused = [False] * len(all_draws)
    i = 0
    while i < len(all_draws):
        j = i
        while j < len(all_draws) and all_draws[j] == all_draws[i]:
            j += 1
        if j - i >= 4:
            paused[i:j] = [True] * (j - i)
        i = j
    first_line = (window[0][0] - 1) // 30 if window else 0
    window = [(t, i, (f - 1) // 30 < len(paused) and paused[(f - 1) // 30]) for f, t, i in window]
    draws = [d for k, d in enumerate(all_draws[first_line:], first_line) if not paused[k]]
    # Guest PPU threads log their host thread id; name them after the guest's.
    text = log_path.read_text(errors='replace')
    guest = dict(re.findall(r'sys_ppu_thread_create tid=(\d+) name="([^"]*)"', text))
    names = {int(h): guest.get(t, 'ppu' + t) for t, h in
             re.findall(r'\[THREAD (\d+)\] host thread started, entry=\S+ hosttid=(\d+)', text)}
    names[proc.pid] = 'main'
    threads = {}
    for tid, (name, ticks) in t1_cpu.items():
        before = t0_cpu[tid][1] if tid in t0_cpu else 0
        threads['%s/%d' % (names.get(tid, name), tid)] = 100.0 * (ticks - before) / CLK_TCK / wall
    if samples:
        (work / 'stacks.txt').write_text('\n\n'.join(samples))
    return window, threads, stack_summary(samples, names) if stacks else {}, draws, unfreezes


def physical_cores():
    """[[logical cpus of one core], ...] in core order."""
    cores = {}
    for cpu in sorted(os.sched_getaffinity(0)):
        sib = Path('/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list' % cpu).read_text().strip()
        cores.setdefault(sib, []).append(cpu)
    return list(cores.values())


def pin_busiest(pid, count):
    """Experiment: the `count` busiest threads (over 3 s) each get a whole physical core,
    the other threads share the remaining cores."""
    a = thread_times(pid)
    time.sleep(3)
    b = thread_times(pid)
    busy = sorted(b, key=lambda t: -(b[t][1] - a.get(t, (0, 0))[1]))[:count]
    cores = physical_cores()
    rest = [c for core in cores[count:] for c in core]
    for tid in b:
        try:
            os.sched_setaffinity(tid, set(cores[busy.index(tid)]) if tid in busy else set(rest))
        except (OSError, ValueError):
            pass


def stack_summary(samples, names):
    """{tid: [(count, innermost frames)]}: what each thread was doing in the snapshots."""
    out = {}
    for text in samples:
        for block in re.split(r'\n(?=TID )', text):
            m = re.match(r'TID (\d+):', block)
            if not m:
                continue
            frames = re.findall(r'#\d+\s+0x[0-9a-f]+\s+(.*)', block)
            frames = [re.sub(r'\s+-\s+.*', '', f).strip() for f in frames]
            # Skip the C library's wait plumbing to show who waits.
            own = [f for f in frames if not re.match(r'(__|#\d|pthread_cond|clock_nanosleep|nanosleep|ioctl)', f)]
            lead = frames[0] if frames else ''
            out.setdefault(m[1], []).append(' < '.join(([lead] if own[:1] != frames[:1] else []) + own[:4]))
    summary = {}
    for key, tops in out.items():
        counts = {}
        for t in tops:
            counts[t] = counts.get(t, 0) + 1
        summary[key] = sorted(((c, t) for t, c in counts.items()), reverse=True)[:5]
    return summary


def summarise(window):
    """Statistics over the frames of `window` [(time, interval, paused)] that
    were not a paused screen; paused_s is how long the rest lasted."""
    moving = [(t, i) for t, i, p in window if not p]
    paused_s = sum(i for _, i, p in window if p) / 1000.0
    if len(moving) < 60:
        raise RuntimeError('the fight stayed paused for the whole window')
    intervals = sorted(i for _, i in moving if i > 0)
    n = len(intervals)
    # The worst 5 s within each stretch of uninterrupted fighting, so that a
    # removed pause does not count as missing frames.
    segments, cur = [], []
    for t, i, p in window:
        if p:
            if cur: segments.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur: segments.append(cur)
    worst5 = [sum(1 for u in seg if s <= u < s + 5000) / 5.0
              for seg in segments if seg[-1] - seg[0] > 5000 for s in seg if s <= seg[-1] - 5000]
    slow = intervals[-max(1, n // 100):]
    return {'frames': len(moving), 'avg_fps': round(1000.0 * n / sum(intervals), 1),
            'paused_s': round(paused_s, 1),
            'p95_ms': round(intervals[int(n * .95)], 1), 'p99_ms': round(intervals[int(n * .99)], 1),
            'over_16_7': sum(i > 16.7 for i in intervals), 'over_33': sum(i > 33.4 for i in intervals),
            'low1_fps': round(1000.0 / statistics.mean(slow), 1),
            'worst5_fps': round(min(worst5), 1) if worst5 else 0.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--binary', type=Path, required=True)
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--seconds', type=float, default=60)
    ap.add_argument('--label', default='')
    ap.add_argument('--json', type=Path)
    ap.add_argument('--dump', type=Path, help='write one fight frame (10 s in) to this PPM file')
    ap.add_argument('--keep-logs', type=Path, help='copy each run\'s game log here')
    ap.add_argument('--prefix', default='', help='command to start the game through, e.g. gamemoderun')
    ap.add_argument('--pin', type=int, default=0, help='experiment: give the N busiest threads a core each')
    ap.add_argument('--stacks', type=int, default=0, help='eu-stack snapshots per run (diagnostics; perturbs timing)')
    ap.add_argument('env', nargs='*', help='extra ENV=VALUE for the game')
    args = ap.parse_args()
    extra = dict(e.split('=', 1) for e in args.env)
    results = []
    for n in range(args.runs):
        # The scripted menu route sometimes stalls before the fight; that run
        # measured nothing, so start it again (twice at most).
        for attempt in range(3):
            with tempfile.TemporaryDirectory(prefix='bench-') as work:
                try:
                    try:
                        window, threads, stacks, draws, unfreezes = run_once(args.binary.resolve(), args.seconds, extra, Path(work),
                                                           args.stacks, args.pin, args.prefix.split(),
                                                           args.dump.resolve() if args.dump else None)
                    finally:
                        if args.keep_logs and (Path(work) / 'run.log').exists():
                            args.keep_logs.mkdir(parents=True, exist_ok=True)
                            shutil.copy2(Path(work) / 'run.log', args.keep_logs / ('run%d.log' % (n + 1)))
                            if (Path(work) / 'stacks.txt').exists():
                                shutil.copy2(Path(work) / 'stacks.txt', args.keep_logs / ('stacks%d.txt' % (n + 1)))
                            if (Path(work) / 'frames.csv').exists():
                                shutil.copy2(Path(work) / 'frames.csv', args.keep_logs / ('frames%d.csv' % (n + 1)))
                    break
                except RuntimeError as e:
                    if attempt == 2 or not ('combat not reached' in str(e) or 'stayed paused' in str(e)):
                        raise
                    print('run %d: %s; retrying' % (n + 1, e), flush=True)
        s = summarise(window)
        s['unfreeze_presses'] = unfreezes
        s['draws'] = {'min': min(draws), 'mean': round(statistics.mean(draws)), 'max': max(draws),
                      'distinct': len(set(draws))} if draws else {}
        s['threads'] = {k: round(v, 1) for k, v in sorted(threads.items(), key=lambda kv: -kv[1])[:6]}
        if stacks:
            s['stacks'] = {k: stacks[k.rsplit('/', 1)[1]] for k in s['threads'] if k.rsplit('/', 1)[1] in stacks}
        results.append(s)
        print('run %d: %.1f fps, p95 %.1f ms, >16.7 ms %d, >33 ms %d, 1%% low %.1f, worst 5 s %.1f, paused %.1f s, draws %s | %s' % (
            n + 1, s['avg_fps'], s['p95_ms'], s['over_16_7'], s['over_33'], s['low1_fps'], s['worst5_fps'], s['paused_s'],
            '%(min)d-%(max)d' % s['draws'] if s['draws'] else '?',
            ', '.join('%s %.0f%%' % kv for kv in list(s['threads'].items())[:4])), flush=True)
    keys = ('avg_fps', 'p95_ms', 'p99_ms', 'over_16_7', 'over_33', 'low1_fps', 'worst5_fps', 'paused_s')
    mean = {k: round(statistics.mean(r[k] for r in results), 1) for k in keys}
    print('mean: ' + ', '.join('%s %s' % kv for kv in mean.items()))
    if args.json:
        args.json.write_text(json.dumps({'label': args.label, 'binary': str(args.binary), 'env': extra,
                                         'seconds': args.seconds, 'runs': results, 'mean': mean}, indent=2) + '\n')


if __name__ == '__main__':
    main()
