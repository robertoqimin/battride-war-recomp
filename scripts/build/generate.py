#!/usr/bin/env python3
"""Generate private PPU/SPU sources from the user's matching decrypted ELF."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
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

# The generated folders, relative to the port root; --cache keeps a copy of them.
GENERATED = ('src/recomp', 'spu', 'analysis/regeneration')
# Bump when what a cache entry holds or how it is restored changes.
CACHE_FORMAT = 1

def cache_key(sdk, elf_digest, dev_flash):
    """Everything the generated sources depend on: the EBOOT, the pinned SDK's
    tools, this script and the audio lifter, the SDK patches, recompile.json, the
    firmware codecs the audio lifter translates, and the Python and Capstone the
    tools run with. --jobs only splits the work and is left out."""
    h = hashlib.sha256()
    def add(label, data):
        h.update(label.encode() + b'\0' + data + b'\0')
    add('format', str(CACHE_FORMAT).encode())
    add('eboot', elf_digest.encode())
    add('python', sys.version.encode())
    try:
        from importlib.metadata import version
        add('capstone', version('capstone').encode())
    except Exception:
        add('capstone', b'unknown')
    files = [ROOT / 'recompile.json', ROOT / 'scripts/build/generate.py', ROOT / 'scripts/build/lift_audio_spu.py',
             ROOT / 'scripts/build/ppu_lift_nv.py', ROOT / 'scripts/build/ppu_nv_dirty.py']
    # The register-locals lift and its checking form give other sources.
    for name in ('PORT_PPU_NV', 'PORT_PPU_NV_VERIFY', 'PPU_NV_LIVE', 'PPU_NV_VRALL', 'PPU_NV_FLAGS'):
        add('env/' + name, os.environ.get(name, '').encode())
    files += sorted((ROOT / 'patches/ps3recomp').glob('*.patch'))
    for f in files:
        add(f.relative_to(ROOT).as_posix(), f.read_bytes())
    for f in sorted(p for p in (sdk / 'tools').rglob('*') if p.is_file() and '__pycache__' not in p.parts):
        add('sdk/' + f.relative_to(sdk).as_posix(), f.read_bytes())
    external = dev_flash / 'sys/external'
    for f in sorted(external.glob('*.pic')) if external.is_dir() else ():
        add('firmware/' + f.name, f.read_bytes())
    return h.hexdigest()

def restore_cached(entry, destinations):
    """Install a cache entry's sources; its spu/sources.cmake names the folder it was made in."""
    info = json.loads((entry / 'complete.json').read_text())
    for name, dest in zip(GENERATED, destinations):
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(entry / name, dest)
    cmake = ROOT / 'spu/sources.cmake'
    cmake.write_text(cmake.read_text().replace(info['spu_dir'], str(ROOT / 'spu')))

def store_cached(cache, key, destinations):
    """Keep a copy of the sources just generated as the only entry in cache."""
    cache.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix='.gen-tmp-', dir=cache))
    try:
        for name, source in zip(GENERATED, destinations):
            shutil.copytree(source, tmp / name)
        (tmp / 'complete.json').write_text(json.dumps({'format': CACHE_FORMAT, 'key': key,
                                                       'spu_dir': str(ROOT / 'spu')}, indent=2))
        entry = cache / ('gen-' + key[:32])
        shutil.rmtree(entry, ignore_errors=True)
        tmp.rename(entry)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for old in cache.iterdir():
        if old.is_dir() and old.name != entry.name and old.name.startswith(('gen-', '.gen-tmp-')):
            shutil.rmtree(old, ignore_errors=True)

# The guest arena's base as a constant in the translated code (port M3). Every
# guest access read vm_base back from memory: stores through uint8_t* may alias
# it, so the compiler reloaded it after each one. When the build maps the arena
# at a fixed address (PORT_VM_FIXED_ADDR, main.cpp) and compiles the chunks
# with PORT_VM_BASE_CONST, vm_base below the declaration is that constant,
# kept in a register; anything else that includes the header is unchanged.
VM_BASE_DECL = 'extern uint8_t* vm_base;'
VM_BASE_CONST = ('#if defined(PORT_VM_BASE_CONST) && defined(PORT_VM_FIXED_ADDR)\n'
                 '#define vm_base ((uint8_t*)(uintptr_t)PORT_VM_FIXED_ADDR)\n'
                 '#endif')

# Each chunk declares the variable again; with the constant that line goes.
CHUNK_VM_BASE_DECL = 'extern "C" uint8_t* vm_base;\n'

def pin_vm_base(ppu):
    path = ppu / 'ppu_recomp.h'
    text = path.read_text()
    if text.count(VM_BASE_DECL + '\n') != 1:
        raise RuntimeError('vm_base constant: expected one "%s" line in ppu_recomp.h' % VM_BASE_DECL)
    path.write_text(text.replace(VM_BASE_DECL + '\n', VM_BASE_DECL + '\n' + VM_BASE_CONST + '\n'))
    for chunk in sorted(ppu.glob('ppu_recomp_*.cpp')):
        with open(chunk, newline='') as f:
            text = f.read()
        n = text.count(CHUNK_VM_BASE_DECL)
        if n > 1:
            raise RuntimeError('vm_base constant: %d declarations in %s' % (n, chunk.name))
        if n:
            with open(chunk, 'w', newline='') as f:
                f.write(text.replace(CHUNK_VM_BASE_DECL,
                                     '#ifndef PORT_VM_BASE_CONST\n' + CHUNK_VM_BASE_DECL + '#endif\n'))

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk', type=Path, default=ROOT.parent / 'ps3recomp')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--verify-only', action='store_true')
    ap.add_argument('--cache', type=Path,
                    help='keep the generated sources here and reuse them when nothing they depend on changed')
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
    destinations = [ROOT / name for name in GENERATED]
    for path in destinations:
        if path.exists() or path.is_symlink():
            ap.error(f'refusing to overwrite {path}; use a fresh source checkout')
    dev_flash = Path(os.environ.get('PS3_DEV_FLASH') or ROOT / 'firmware/dev_flash')
    key = entry = None
    if args.cache:
        key = cache_key(sdk, digest, dev_flash)
        entry = args.cache.resolve() / ('gen-' + key[:32])
        try:
            if json.loads((entry / 'complete.json').read_text()).get('key') == key:
                restore_cached(entry, destinations)
                print('Reused the sources translated earlier (%s); nothing they depend on changed.' % entry.name)
                return
        except (OSError, ValueError):
            for path in destinations:
                shutil.rmtree(path, ignore_errors=True)
    with tempfile.TemporaryDirectory(prefix='.generate-', dir=ROOT) as name:
        stage = Path(name)
        ppu = stage / 'src/recomp'
        spu = stage / 'spu'
        analysis = stage / 'analysis'
        for path in (ppu, spu, analysis):
            path.mkdir(parents=True)
        run([sys.executable, sdk / 'tools/ppu_loader.py', elf, '-o', analysis])
        # PORT_PPU_NV=1: the SDK's lifter with the guest registers as C locals
        # (scripts/build/ppu_lift_nv.py); the same functions and chunks.
        if os.environ.get('PORT_PPU_NV', '0') not in ('', '0'):
            lifter = [ROOT / 'scripts/build/ppu_lift_nv.py', '--sdk-tools', sdk / 'tools']
        else:
            lifter = [sdk / 'tools/ppu_lifter.py']
        run([sys.executable, *lifter, elf,
             '--functions', analysis / 'EBOOT.functions.json',
             '--hle-stubs', analysis / 'EBOOT.imports.json', '--output', ppu,
             '--jobs', args.jobs, '--chunk-lines', '80000'])
        hook_graphics_buffers(ppu)
        pin_vm_base(ppu)
        run([sys.executable, sdk / 'tools/build_spu_workloads.py', '--input', elf,
             '--lifted', spu, '--out', spu / 'spu_workloads.c', '--constructor', '--allow-partial'])
        inventory = json.loads((spu / 'inventory.json').read_text())
        registered = {x['sha256'] for x in inventory['programs'] if x['status'] == 'registered'}
        if registered != set(spec['registered_spu_sha256']):
            raise RuntimeError('unexpected SPU registry; generated sources were not installed')
        # The audio task (and, with the firmware present, its MP3 codec):
        # rejected by the generic pipeline, translated below its data tail.
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
    if args.cache:
        try:
            store_cached(args.cache.resolve(), key, destinations)
        except OSError as exc:
            print(f'Could not keep a copy in {args.cache} ({exc}); the next update translates again.')
    print('Generated sources installed locally; the audio SPU task is translated below its data tail.')
    print('Do not publish game/, src/recomp/, spu/ or analysis/.')

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'generation failed: {exc}', file=sys.stderr)
        sys.exit(1)
