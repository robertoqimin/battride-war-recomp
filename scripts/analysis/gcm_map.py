#!/usr/bin/env python3
"""Map the game's renderer from its translation (docs/PLANO0.6.1.md, N3).

usage: gcm_map.py [--recomp src/recomp] [--sdk ../ps3recomp] [--json out.json]

The game writes RSX commands straight into its command buffer: a method
header is (count << 18) | method, built in PPU code by `lis rA, count<<2`
and `ori rB, rA, method`. In the translated C++ that is a register set to
`(uint32_t)0xNNNN << 16` and later OR-ed with a method offset. This finds,
for every translated function, the NV4097 methods it emits that way, and the
functions that call it directly, so the emitters can be grouped into the
layers N3 replaces: frame, pass (surfaces, clears), material (shaders,
textures, blend/depth state) and draw (BEGIN_END, DRAW_ARRAYS/INDEX).

Reads only the generated sources on this computer; prints addresses and
method names, nothing of the game itself.
"""
import argparse
import collections
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FUNC = re.compile(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{$')
LIS = re.compile(r'ctx->gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\(\(uint32_t\)0x([0-9A-F]+) << 16\);')
ORI = re.compile(r'ctx->gpr\[(\d+)\] = ctx->gpr\[(\d+)\] \| 0x([0-9A-F]+);')
CALL = re.compile(r'\bfunc_([0-9A-F]{8})\(ctx\)')
# The layer each method belongs to, for the summary.
LAYERS = (
    ('draw', ('BEGIN_END', 'DRAW_ARRAYS', 'DRAW_INDEX', 'INLINE_ARRAY', 'INDEX_ARRAY', 'VERTEX_DATA_ARRAY')),
    ('pass', ('SURFACE', 'CLEAR', 'COLOR_TARGET', 'CONTEXT_DMA_COLOR', 'CONTEXT_DMA_ZETA', 'WINDOW_OFFSET',
              'VIEWPORT', 'SCISSOR')),
    ('material', ('SHADER', 'TRANSFORM_PROGRAM', 'TRANSFORM_CONSTANT', 'TEXTURE', 'BLEND', 'DEPTH', 'STENCIL',
                  'ALPHA', 'CULL', 'COLOR_MASK', 'FRONT_FACE', 'LOGIC_OP', 'FOG', 'POINT', 'POLYGON')),
    ('sync', ('SEMAPHORE', 'REFERENCE', 'NOTIFY', 'WAIT_FOR_IDLE', 'FLIP', 'JUMP', 'CALL', 'RETURN',
              'NO_OPERATION', 'CONTEXT_DMA')),
)


def method_names(sdk: Path) -> dict:
    names = {}
    for line in (sdk / 'libs/video/rsx_commands.h').read_text().splitlines():
        m = re.match(r'#define NV4097_(\w+)\s+0x([0-9A-Fa-f]+)', line)
        if m:
            names[int(m.group(2), 16)] = m.group(1)
    return names


def layer(name: str) -> str:
    for layer_name, keys in LAYERS:
        if any(k in name for k in keys):
            return layer_name
    return 'other'


def scan(recomp: Path, names: dict) -> dict:
    funcs = {}
    for path in sorted(recomp.glob('ppu_recomp_*.cpp')):
        cur = None
        lis = {}
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line.rstrip('\n'))
            if m:
                cur = {'methods': collections.Counter(), 'calls': set(), 'chunk': path.stem[-3:]}
                funcs[int(m.group(1), 16)] = cur
                lis = {}
                continue
            if cur is None:
                continue
            m = LIS.search(line)
            if m:
                imm = int(m.group(2), 16)
                # count << 2 in the high half (1..2047 words), optionally with the
                # non-incrementing flag (0x4000 << 16).
                if imm & 3 == 0 and (4 <= imm <= 0x1FFC or 0x4004 <= imm <= 0x5FFC):
                    lis[m.group(1)] = imm
                else:
                    lis.pop(m.group(1), None)
                continue
            m = ORI.search(line)
            if m and m.group(2) in lis:
                method = int(m.group(3), 16)
                if method in names:
                    cur['methods'][names[method]] += 1
            for c in CALL.findall(line):
                cur['calls'].add(int(c, 16))
    return funcs


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--recomp', type=Path, default=ROOT / 'src/recomp')
    ap.add_argument('--sdk', type=Path, default=ROOT.parent / 'ps3recomp')
    ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    names = method_names(args.sdk)
    funcs = scan(args.recomp, names)
    callers = collections.defaultdict(set)
    for a, f in funcs.items():
        for c in f['calls']:
            callers[c].add(a)
    emitters = {a: f for a, f in funcs.items() if f['methods']}
    by_layer = collections.Counter()
    for a, f in emitters.items():
        for name in f['methods']:
            by_layer[layer(name)] += 1
    print(f'{len(funcs)} functions, {len(emitters)} emit RSX methods ({sum(by_layer.values())} method sites)')
    for name, count in by_layer.most_common():
        print(f'  {name:9s} {count} sites')
    for lay, _ in LAYERS:
        rows = sorted((a for a, f in emitters.items() if any(layer(n) == lay for n in f['methods'])),
                      key=lambda a: -sum(funcs[a]['methods'].values()))
        print(f'\n{lay}: {len(rows)} functions')
        for a in rows[:12]:
            f = funcs[a]
            ms = ', '.join(f'{n}' for n, _ in f['methods'].most_common(6))
            print(f'  func_{a:08X} (chunk {f["chunk"]}, {len(callers[a])} callers): {ms}')
    if args.json:
        args.json.write_text(json.dumps({f'{a:08X}': {'methods': dict(f['methods']),
                                                       'callers': sorted(f'{c:08X}' for c in callers[a]),
                                                       'chunk': f['chunk']}
                                         for a, f in emitters.items()}, indent=1, sort_keys=True))


if __name__ == '__main__':
    main()
