#!/usr/bin/env python3
"""Generate private PPU/SPU sources from the user's matching decrypted ELF."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def run(args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)

# The renderer setup call where stubs.cpp's port_expand_graphics_buffers
# records the title's two graphics command arenas, located by the guest return
# address of the call that follows it. Only the address is kept here, so the
# repository holds no translated game code.
ARENA_HOOK_CHUNK = 'ppu_recomp_015.cpp'
ARENA_HOOK_RETURN = 'ctx->lr = 0x0045BC64;'

def hook_graphics_buffers(ppu):
    path = ppu / ARENA_HOOK_CHUNK
    with open(path, newline='') as f:
        lines = f.read().split('\n')
    hits = [i for i, line in enumerate(lines) if ARENA_HOOK_RETURN in line]
    if len(hits) != 1:
        raise RuntimeError('graphics buffer hook: expected one call returning to 0x0045BC64 in %s, found %d'
                           % (ARENA_HOOK_CHUNK, len(hits)))
    line = lines[hits[0]]
    indent = line[:len(line) - len(line.lstrip())]
    lines.insert(hits[0], indent + 'port_expand_graphics_buffers(ctx, (uint32_t)ctx->gpr[31]);')
    lines.insert(0, 'extern "C" void port_expand_graphics_buffers(struct ppu_context*, unsigned int);')
    with open(path, 'w', newline='') as f:
        f.write('\n'.join(lines))

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk', type=Path, default=ROOT.parent / 'ps3recomp')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--verify-only', action='store_true')
    args = ap.parse_args()
    spec = json.loads((ROOT / 'recompile.json').read_text())
    sdk = args.sdk.resolve()
    stamp = sdk / '.ps3recomp-commit'
    if not (sdk / '.git').exists() and stamp.is_file():
        # A `git archive` export of the pinned commit, as the AppImage ships.
        if stamp.read_text().strip() != spec['sdk_commit']:
            ap.error('SDK export differs from recompile.json; use the pinned commit')
    else:
        rev = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
        if rev != spec['sdk_commit']:
            ap.error('SDK commit differs from recompile.json; use the pinned checkout')
        if subprocess.run(['git', '-C', str(sdk), 'diff', '--quiet', 'HEAD', '--', 'tools']).returncode:
            ap.error('SDK tools have local modifications; use unmodified pinned tools')
    elf = ROOT / 'game/EBOOT.ELF'
    if not elf.is_file() or elf.is_symlink():
        ap.error('place the matching decrypted file at game/EBOOT.ELF (regular file)')
    with elf.open('rb') as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
        digest = value.hexdigest()
    if digest != spec['eboot_sha256']:
        ap.error('EBOOT SHA-256 mismatch: this port is specific to the documented BLJS10220 build')
    if args.jobs < 1:
        ap.error('--jobs must be positive')
    if args.verify_only:
        print('OK: pinned SDK and matching EBOOT; no files generated')
        return
    destinations = [ROOT / 'src/recomp', ROOT / 'spu', ROOT / 'analysis/regeneration']
    for path in destinations:
        if path.exists() or path.is_symlink():
            ap.error(f'refusing to overwrite {path}; use a fresh source checkout')
    with tempfile.TemporaryDirectory(prefix='.generate-', dir=ROOT) as name:
        stage = Path(name)
        ppu = stage / 'src/recomp'
        spu = stage / 'spu'
        analysis = stage / 'analysis'
        for path in (ppu, spu, analysis):
            path.mkdir(parents=True)
        run([sys.executable, sdk / 'tools/ppu_loader.py', elf, '-o', analysis])
        run([sys.executable, sdk / 'tools/ppu_lifter.py', elf,
             '--functions', analysis / 'EBOOT.functions.json',
             '--hle-stubs', analysis / 'EBOOT.imports.json', '--output', ppu,
             '--jobs', args.jobs, '--chunk-lines', '80000'])
        hook_graphics_buffers(ppu)
        run([sys.executable, sdk / 'tools/build_spu_workloads.py', '--input', elf,
             '--lifted', spu, '--out', spu / 'spu_workloads.c', '--constructor', '--allow-partial'])
        inventory = json.loads((spu / 'inventory.json').read_text())
        registered = {x['sha256'] for x in inventory['programs'] if x['status'] == 'registered'}
        if registered != set(spec['registered_spu_sha256']):
            raise RuntimeError('unexpected SPU registry; generated sources were not installed')
        # The audio task (and, with the firmware present, its MP3 codec):
        # rejected by the generic pipeline, translated below its data tail.
        dev_flash = Path(os.environ.get('PS3_DEV_FLASH') or ROOT / 'firmware/dev_flash')
        audio = [sys.executable, ROOT / 'scripts/build/lift_audio_spu.py', '--sdk', sdk,
                 '--elf', elf, '--spu', spu]
        if dev_flash.is_dir():
            audio += ['--dev-flash', dev_flash]
        run(audio)
        if len(list(ppu.glob('ppu_recomp_*.cpp'))) != spec['ppu_chunks']:
            raise RuntimeError('unexpected PPU chunk layout; generated sources were not installed')
        # Keep the generated CMake source list useful after moving the stage.
        cmake = spu / 'sources.cmake'
        cmake.write_text(cmake.read_text().replace(str(spu), str(ROOT / 'spu')))
        # Recheck before installing. Never replace existing private outputs.
        for path in destinations:
            if path.exists() or path.is_symlink():
                raise RuntimeError(f'destination appeared during generation: {path}')
            path.parent.mkdir(parents=True, exist_ok=True)
        for source, dest in zip((ppu, spu, analysis), destinations):
            source.rename(dest)
    print('Generated sources installed locally; the audio SPU task is translated below its data tail.')
    print('Do not publish game/, src/recomp/, spu/ or analysis/.')

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'generation failed: {exc}', file=sys.stderr)
        sys.exit(1)
