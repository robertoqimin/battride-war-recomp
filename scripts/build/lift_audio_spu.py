#!/usr/bin/env python3
"""Statically translate the audio SPU task (and its MP3 codec) for spu_bridge.c.

Interpreted, the audio task filled about a third of the cellAudio blocks in
time; the sound broke up into short bursts. Two pieces of code do the work:

- the task itself. The static translator rejects the whole image because its
  .text ends in a data tail (473 words from LS 0x171D0 that do not decode);
  everything below that address is code, and is translated on its own.
- flashMP3.pic from the firmware (docs/FIRMWARE.md), which the task loads
  at LS 0x1A880 byte for byte, with no relocation, and runs for most sound
  effects. It is translated from the builder's own copy, only when that copy
  is the one this was checked against, and the translation stays local.
- a 10 KB SPU module embedded in the executable, which the task loads at LS
  0x36F80 the same way. Some stages run it on every audio block (chapter 4,
  stg08); interpreted, it took 29-39 ms a run and most blocks came too late.

spu_bridge.c runs a translated function when the interpreter reaches one. Calls
inside translated code return through the port's own drain (port_audio_drain in
spu_bridge.c, put here in place of the runtime's spu_drain_call): the runtime's
would end the task on code it has no translation for, while the port's
interprets that code until it returns. Only a function with a word the
translator cannot handle stays interpreted. A codec function also runs only
while local store still holds the codec's own instructions there.

Writes <spu>/battride_audio/{task,mp3,module}/spu_recomp.[ch] and audio_task.h.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

TASK_SHA256 = 'e33837bb55e56b6d6ae06bac574b8fa614976a6c4cf40b0b98348c868d490c53'
TASK_SIZE = 95636
TASK_CODE_END = 0x171D0
MP3_BASE = 0x1A880             # where the task loads it (vaddr 0)
# The flashMP3.pic versions this was checked against, by SHA-256: (link
# addresses of the code segment, its file offset). Another firmware's copy is
# added here once checked; until then it stays interpreted
# (docs/PLANO0.6.1.md, N1).
MP3_VERSIONS = {
    'adbd993483a41f53c994e080665fb5fbf36af783956d0e4ded30057b8b4e894a': ((0x80, 0xFF90), 0x100),
}
MODULE_SHA256 = '615f01318fb1c02fbb03cd0fdca61b2144aaed4846bb2fc12fe4ca8fe2dc0657'
MODULE_SIZE = 0x2A4C
MODULE_BASE = 0x36F80
MODULE_TEXT = (0x80, 0x2840)
MODULE_TEXT_OFFSET = 0x80


def find_image(elf: Path, sha256: str, size: int, what: str) -> bytes:
    data = elf.read_bytes()
    pos = 0
    while (pos := data.find(b'\x7fELF\x01\x02\x01', pos + 1)) > 0:
        blob = data[pos:pos + size]
        if hashlib.sha256(blob).hexdigest() == sha256:
            return blob
    raise SystemExit(f'{what} {sha256[:16]}... not found in {elf}')


def lift(sdk: Path, image: Path, out: Path, prefix: str, extra: list[str]) -> str:
    out.mkdir(parents=True)
    subprocess.run([sys.executable, str(sdk / 'tools/spu_lifter.py'),
                    '--auto-functions', str(image), '--merge-chunks',
                    '--symbol-prefix', prefix, '--output', str(out),
                    '--report', str(out / 'report.json'), *extra], check=True)
    path = out / 'spu_recomp.c'
    path.write_text(own_drains(path.read_text()))
    return path.read_text()


# A translated call: the callee (direct, or the runtime's indirect resolver),
# then the drain back to the return address.
INDIRECT_CALL = re.compile(r'spu_indirect_branch\(ctx\); spu_drain_call\(ctx, (0x[0-9A-Fa-f]+)\);')
CALL_DRAIN = re.compile(r'spu_drain_call\(ctx, (0x[0-9A-Fa-f]+)\);')


def own_drains(source: str) -> str:
    """The translation with every call returning through port_audio_drain."""
    source = INDIRECT_CALL.sub(r'port_audio_drain(ctx, \1);', source)
    source = CALL_DRAIN.sub(r'port_audio_drain(ctx, \1);', source)
    head, sep, rest = source.partition('\n\n')
    return head + '\nvoid port_audio_drain(spu_context* ctx, uint32_t return_pc);' + sep + rest


def compiled_entries(source: str, prefix: str) -> tuple[list[int], list[int]]:
    """(entries safe to run, entries to keep interpreting), for a translation
    after own_drains.

    Every call and transfer, direct or indirect, ends in port_audio_drain or in
    a queued trampoline that spu_bridge.c follows, both of which interpret
    what has no translation. What remains unsafe is a word the translator
    could not handle (spu_unsupported) and any call left to the runtime's
    resolver, in the function or in a callee it calls."""
    bodies = {int(m.group(1), 16): m.group(2) for m in re.finditer(
        rf'^void {prefix}spu_func_([0-9A-F]{{8}})\(spu_context\* ctx\) \{{\n(.*?)^\}}\n',
        source, re.S | re.M)}
    if not bodies:
        raise SystemExit(f'no translated {prefix} functions found')
    calls = {a: {int(x, 16) for x in re.findall(rf'{prefix}spu_func_([0-9A-F]{{8}})\(ctx\)', b)}
             for a, b in bodies.items()}
    tails = {a: {int(x, 16) for x in re.findall(rf'g_spu_trampoline_fn = {prefix}spu_func_([0-9A-F]{{8}})', b)}
             for a, b in bodies.items()}
    call_ib = {a: 'spu_indirect_branch(ctx)' in b or 'spu_drain_call(' in b or 'spu_unsupported' in b
               for a, b in bodies.items()}
    memo: dict[int, bool] = {}

    def nested_bad(a: int, stack: frozenset = frozenset()) -> bool:
        if a in memo:
            return memo[a]
        if a in stack:
            return False
        if a not in bodies:
            memo[a] = True
            return True
        bad = call_ib[a] or any(nested_bad(x, stack | {a}) for x in calls[a] | tails[a])
        memo[a] = bad
        return bad

    sys.setrecursionlimit(20000)
    keep = sorted(a for a in bodies if call_ib[a] or any(nested_bad(x) for x in calls[a]))
    return sorted(set(bodies) - set(keep)), keep


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sdk', required=True, type=Path, help='ps3recomp checkout')
    ap.add_argument('--elf', required=True, type=Path, help='decrypted game/EBOOT.ELF')
    ap.add_argument('--spu', required=True, type=Path, help='generated spu/ directory')
    ap.add_argument('--dev-flash', type=Path, help='dev_flash root holding sys/external/flashMP3.pic')
    args = ap.parse_args()

    out = args.spu / 'battride_audio'
    if out.exists():
        raise SystemExit(f'refusing to overwrite {out}')
    out.mkdir(parents=True)

    image = out / 'audio_task.elf'
    image.write_bytes(find_image(args.elf, TASK_SHA256, TASK_SIZE, 'audio SPU task'))
    task_src = lift(args.sdk, image, out / 'task', 'spu_audio_', ['--code-end', hex(TASK_CODE_END)])
    image.unlink()      # game data; only the translation is needed
    if json.loads((out / 'task/report.json').read_text()).get('unsupported'):
        raise SystemExit('unsupported instructions below the task data tail')
    _, task_keep = compiled_entries(task_src, 'spu_audio_')
    lines = ['/* GENERATED by scripts/build/lift_audio_spu.py -- do not edit, do not publish. */',
             '#define PORT_AUDIO_LIFTED 1',
             f'#define PORT_AUDIO_TASK_CODE_END 0x{TASK_CODE_END:X}u',
             'void spu_audio_spu_recomp_register(void);',
             '/* Translated task entries that stay interpreted (sorted). */',
             'static const uint32_t port_audio_interpret_only[] = {',
             *(f'    0x{a:05X}u,' for a in task_keep), '};']
    summary = f'task: {len(task_keep)} entries left interpreted'

    overlays = [('module', find_image(args.elf, MODULE_SHA256, MODULE_SIZE, 'audio SPU module'),
                 MODULE_BASE, MODULE_TEXT, MODULE_TEXT_OFFSET)]
    codec = args.dev_flash / 'sys/external/flashMP3.pic' if args.dev_flash else None
    version = MP3_VERSIONS.get(hashlib.sha256(codec.read_bytes()).hexdigest()) \
        if codec and codec.is_file() else None
    if version:
        overlays.insert(0, ('mp3', codec.read_bytes(), MP3_BASE, version[0], version[1]))
    else:
        summary += '; flashMP3.pic: not found or not the checked version, stays interpreted'

    # Overlay entries run translated, each only while local store holds the
    # instructions it was translated from (another overlay may sit there).
    entries, ranges = [], []
    for name, code, base, text, text_offset in overlays:
        elf = out / f'{name}.elf'
        elf.write_bytes(code)
        rebased = out / f'{name}.rebased.elf'
        subprocess.run([sys.executable, str(args.sdk / 'tools/rebase_spu_elf.py'),
                        str(elf), str(rebased), hex(base)], check=True)
        src = lift(args.sdk, rebased, out / name, f'spu_{name}_', [])
        elf.unlink()
        rebased.unlink()
        run, _ = compiled_entries(src, f'spu_{name}_')
        lo, hi = base + text[0], base + text[1]

        def words(pc: int) -> str:
            off = text_offset + pc - lo
            return ', '.join(f'0x{int.from_bytes(code[off + 4 * i:off + 4 * i + 4], "big"):08X}u'
                             for i in range(4))
        run = [a for a in run if lo <= a and a + 16 <= hi]
        entries += [f'    {{ 0x{a:05X}u, {{ {words(a)} }} }},' for a in run]
        ranges.append(f'    {{ 0x{lo:05X}u, 0x{hi:05X}u }},')
        lines.append(f'void spu_{name}_spu_recomp_register(void);')
        summary += f'; {name}: {len(run)} entries translated'
    lines += ['#define PORT_AUDIO_CODEC 1',
              '/* Local-store ranges of the translated overlays. */',
              'static const struct { uint32_t lo, hi; } port_audio_codec_ranges[] = {', *ranges, '};',
              '/* Overlay entries run translated, with the instructions expected there (sorted). */',
              'static const struct { uint32_t pc, w[4]; } port_audio_codec_entries[] = {',
              *sorted(entries), '};',
              'static void port_audio_codec_register(void)', '{',
              *(f'    spu_{name}_spu_recomp_register();' for name, *_ in overlays), '}']
    (out / 'audio_task.h').write_text('\n'.join(lines) + '\n')
    print('audio SPU ' + summary)


if __name__ == '__main__':
    main()
