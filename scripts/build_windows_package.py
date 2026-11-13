#!/usr/bin/env python3
"""Assemble the Windows package the launcher runs from (packaging/launcher.py).

The same layout as the AppImage (app/, usr/bin, usr/share/battride), with
Windows tools: 7-Zip, FFmpeg and graphics-probe.exe. Like the AppImage it holds
no game code or assets: the disc manifest has only names, sizes and SHA-256 of
the files the import verifies.

With --bundle the package carries its own MSYS2 UCRT64 subset under
usr/toolchain/ucrt64: the compilers, CMake, Ninja, the SDK's libraries, and one
Python with GTK 4, libadwaita and PyGObject for the launcher and capstone and
pycryptodome for the SDK tools (the only modules of tools/requirements.txt they
import). The packages and every dependency come from the local pacman database.
Without it, usr/toolchain/toolchain.json points at the local installation and at
--python, and the package runs only on the computer that built it.

usage: build_windows_package.py --stage <new dir> --sevenzip-dir <dir with 7z.exe and 7z.dll>
       (--bundle | --python <python> --ffmpeg-dir <dir with ffmpeg.exe and ffprobe.exe>)
       [--pgo <profiles from scripts/pgo_train.py>] [--toolchain C:/msys64/ucrt64] [--sdk ../ps3recomp]

With --pgo the package carries GCC profiles that the player's build applies
(PORT_PGO=use). They fit only the GCC, SDK and EBOOT they were trained with.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

P = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--stage', type=Path, required=True)
ap.add_argument('--sevenzip-dir', type=Path, required=True)
ap.add_argument('--ffmpeg-dir', type=Path, help="FFmpeg for the movies (without --bundle, which takes MSYS2's)")
ap.add_argument('--python', type=Path, help='Python with the SDK tools\' requirements (without --bundle)')
ap.add_argument('--bundle', action='store_true', help='copy the MSYS2 toolchain, GTK and Python into the package')
ap.add_argument('--pgo', type=Path, help='GCC profiles and profile.json from scripts/pgo_train.py')
ap.add_argument('--toolchain', type=Path, default=Path('C:/msys64/ucrt64'))
ap.add_argument('--sdk', type=Path, default=P.parent / 'ps3recomp')
args = ap.parse_args()
if args.bundle == bool(args.python):
    ap.error('give either --bundle or --python')
if args.bundle == bool(args.ffmpeg_dir):
    ap.error('--ffmpeg-dir goes with --python; --bundle takes FFmpeg from MSYS2')
# What the package needs from MSYS2 UCRT64; pacman adds their dependencies.
BUNDLE = ('gcc', 'clang', 'lld', 'binutils', 'cmake', 'ninja', 'pkgconf', 'SDL2', 'vulkan-headers', 'glslang',
          'spirv-tools', 'python', 'python-capstone', 'python-pycryptodome', 'python-gobject', 'gtk4',
          'libadwaita', 'adwaita-icon-theme', 'ffmpeg')
# Not needed to build or to show the launcher.
SKIP_DIRS = ('share/doc/', 'share/man/', 'share/info/', 'share/gtk-doc/', 'share/locale/')
S = args.stage.resolve()
if S.exists():
    raise SystemExit('Choose a new stage directory; existing files are preserved.')
tools = args.toolchain.resolve() / 'bin'
for exe in ('gcc.exe', 'clang.exe', 'g++.exe', 'cmake.exe', 'ninja.exe', 'objdump.exe', 'python.exe'):
    if not (tools / exe).is_file():
        raise SystemExit('MSYS2 UCRT64 tool missing: ' + str(tools / exe))
spec = json.loads((P / 'recompile.json').read_text())


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def copy(a, b):
    b.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(a, b)


# The launcher and the port's public sources, exactly PUBLIC_FILES.txt.
for n in ('launcher.py', 'importer.py'):
    copy(P / 'packaging' / n, S / 'app' / n)
copy(P / 'CREDITS.md', S / 'app/CREDITS.md')
for n in (P / 'PUBLIC_FILES.txt').read_text().split():
    copy(P / n, S / 'usr/share/battride/port' / n)

# The pinned SDK with the port's patches, as a git export (no .git; generate.py reads the stamp).
print('Exporting the pinned ps3recomp SDK...', flush=True)
archive = subprocess.check_output(['git', '-C', str(args.sdk), '-c', 'core.autocrlf=false', 'archive',
                                   spec['sdk_commit']])
sdk = S / 'usr/share/battride/ps3recomp'
sdk.mkdir(parents=True)
with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
    tar.extractall(sdk, filter='data')
(sdk / '.ps3recomp-commit').write_text(spec['sdk_commit'] + '\n')
for patch in sorted((P / 'patches/ps3recomp').glob('*.patch')):
    # core.autocrlf=false: Git for Windows would otherwise write the patched files with CRLF.
    subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', str(patch)], cwd=sdk, check=True,
                   env={**os.environ, 'GIT_CEILING_DIRECTORIES': str(sdk.parent)})

# 7-Zip, FFmpeg, and the Vulkan/window check with the SDL2 DLLs it needs.
for n in ('7z.exe', '7z.dll'):
    copy(args.sevenzip_dir / n, S / 'usr/bin' / n)
copy(args.sevenzip_dir / 'License.txt', S / 'usr/share/licenses/7zip/License.txt')
if args.ffmpeg_dir:
    for n in ('ffmpeg.exe', 'ffprobe.exe'):
        copy(args.ffmpeg_dir / n, S / 'usr/bin' / n)
env = {**os.environ, 'PATH': os.pathsep.join([str(tools), os.environ.get('PATH', '')])}
subprocess.run([str(tools / 'gcc.exe'), '-O2', '-msse4.1', '-DSDL_MAIN_HANDLED', str(P / 'packaging/graphics_probe.c'),
                '-I' + str(args.toolchain / 'include/SDL2'), '-o', str(S / 'usr/bin/graphics-probe.exe'),
                '-lSDL2', '-lvulkan-1'], check=True, env=env)
found, todo = set(), [S / 'usr/bin/graphics-probe.exe']
while todo:
    text = subprocess.run([str(tools / 'objdump.exe'), '-p', str(todo.pop())], capture_output=True, text=True,
                          env=env).stdout
    for line in text.splitlines():
        name = line.split('DLL Name: ', 1)[1].strip() if 'DLL Name: ' in line else ''
        if name and name.lower() not in found and (tools / name).is_file() and not name.lower().startswith('vulkan-1'):
            found.add(name.lower())
            copy(tools / name, S / 'usr/bin' / name)
            todo.append(tools / name)

(S / 'usr/toolchain').mkdir(parents=True)
if args.bundle:
    # Every file of the packages and of their dependencies, at the same place
    # below usr/toolchain/ucrt64 as below the MSYS2 ucrt64 directory.
    pacman = Path(args.toolchain).resolve().parent / 'usr/bin/pacman.exe'
    msys_root = Path(args.toolchain).resolve().parent
    todo, seen, packages = ['mingw-w64-ucrt-x86_64-' + n for n in BUNDLE], set(), set()
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        info = subprocess.run([str(pacman), '-Qi', name], capture_output=True, text=True, check=True).stdout
        # A dependency may name what a package provides (fc-libs is fontconfig's): keep the package.
        packages.add(next(l.split(':', 1)[1].strip() for l in info.splitlines() if l.startswith('Name')))
        depends = next((l.split(':', 1)[1] for l in info.splitlines() if l.startswith('Depends On')), '')
        for dep in depends.split():
            dep = dep.split('>')[0].split('<')[0].split('=')[0]
            if dep.startswith('mingw-w64-ucrt-x86_64-') and dep not in seen:
                todo.append(dep)
    copied, size = 0, 0
    for name in sorted(packages):
        listing = subprocess.run([str(pacman), '-Ql', name], capture_output=True, text=True, check=True).stdout
        for line in listing.splitlines():
            rel = line.split(' ', 1)[1].lstrip('/')
            if not rel.startswith('ucrt64/') or rel.endswith('/'):
                continue
            inside = rel[len('ucrt64/'):]
            if inside.startswith(SKIP_DIRS) or '/__pycache__/' in inside or '/site-packages/' not in inside and '/test/' in inside:
                continue
            src = msys_root / rel
            if src.is_file():
                copy(src, S / 'usr/toolchain/ucrt64' / inside)
                copied += 1
                size += src.stat().st_size
    # The caches pacman's install hooks generate, which no package lists: GTK's
    # file chooser aborts without the compiled GSettings schemas.
    u = S / 'usr/toolchain/ucrt64'
    hook_env = {**os.environ, 'PATH': os.pathsep.join([str(u / 'bin'), os.environ.get('PATH', '')])}
    for cmd in (['glib-compile-schemas', u / 'share/glib-2.0/schemas'],
                ['gdk-pixbuf-query-loaders', '--update-cache'],
                ['gtk4-update-icon-cache', '-q', '-f', '-t', u / 'share/icons/Adwaita'],
                ['gtk4-update-icon-cache', '-q', '-f', '-t', u / 'share/icons/hicolor'],
                ['update-mime-database', u / 'share/mime']):
        subprocess.run([str(u / 'bin' / (cmd[0] + '.exe')), *map(str, cmd[1:])], check=True, env=hook_env)
    # Name, version and source package of each, for scripts/collect_windows_sources.py.
    listed = []
    for name in sorted(packages):
        desc = next((msys_root / 'var/lib/pacman/local').glob(name + '-[0-9]*/desc'), None)
        fields = dict(re.findall(r'%([A-Z]+)%\n([^\n]*)', desc.read_text(encoding='utf-8'))) if desc else {}
        if fields.get('NAME') != name:
            raise SystemExit('pacman database entry not found for ' + name)
        listed.append({'name': name, 'version': fields['VERSION'], 'base': fields.get('BASE', name),
                       'license': fields.get('LICENSE', '')})
    (S / 'usr/toolchain/packages.json').write_text(json.dumps(listed, indent=1) + '\n')
    print('Toolchain: %d MSYS2 packages, %d files, %.0f MB' % (len(packages), copied, size / 1e6), flush=True)
    toolchain = {'bin': 'ucrt64/bin', 'python': 'ucrt64/bin/python.exe'}
else:
    # This computer's MSYS2 and the given Python.
    toolchain = {'bin': str(tools), 'python': str(args.python.resolve())}
(S / 'usr/toolchain/toolchain.json').write_text(json.dumps(toolchain, indent=2) + '\n')

pgo = False
if args.pgo:
    info = json.loads((args.pgo / 'profile.json').read_text())
    gcc = subprocess.check_output([str(tools / 'g++.exe'), '-dumpfullversion'], text=True).strip()
    if (info['gcc'], info['sdk_commit'], info['eboot_sha256']) != (gcc, spec['sdk_commit'], spec['eboot_sha256']):
        raise SystemExit('The profiles in %s were trained with GCC %s, SDK %s; this build uses GCC %s, SDK %s.'
                         % (args.pgo, info['gcc'], info['sdk_commit'], gcc, spec['sdk_commit']))
    files = sorted(args.pgo.rglob('*.gcda'))
    if len(files) != info['files']:
        raise SystemExit('profile.json lists %d profiles, found %d' % (info['files'], len(files)))
    # As in build_appimage.py: SDK sources keep the training SDK path in their
    # names (C:/x/ps3recomp as C_#x#ps3recomp); ship them under a neutral one,
    # which the importer renames to the player's SDK copy.
    trained = info.get('sdk_dir', '').replace(':', '_').replace('/', '#')
    neutral = '/ps3recomp'
    for f in files:
        name = f.relative_to(args.pgo).as_posix()
        if '/' in name:
            raise SystemExit('Profile below a directory (trained before the CMake prefix fix?): ' + name)
        if trained:
            name = name.replace(trained, neutral.replace('/', '#'))
        copy(f, S / 'usr/share/battride/pgo' / name)
    (S / 'usr/share/battride/pgo/profile.json').write_text(json.dumps(dict(info, sdk_dir=neutral), indent=2) + '\n')
    if any('Users#' in n.name for n in (S / 'usr/share/battride/pgo').iterdir()):
        raise SystemExit('A profile name still carries a user directory.')
    pgo = True

# What the import verifies on the player's disc: names, sizes and SHA-256 only.
manifest = {}
for p in sorted((P / 'game/PS3_GAME').rglob('*')):
    if p.is_file() and p.suffix.lower() != '.elf':
        manifest[p.relative_to(P / 'game').as_posix()] = {'size': p.stat().st_size, 'sha256': sha(p)}
p = P / 'game/PS3_DISC.SFB'
if p.exists():
    manifest['PS3_DISC.SFB'] = {'size': p.stat().st_size, 'sha256': sha(p)}
(S / 'app/disc-manifest.json').write_text(json.dumps(manifest, separators=(',', ':')))
meta = {'version': '0.5.2-windows', 'cpu': 'built on the player computer, -march=native',
        'elf_sha256': spec['eboot_sha256'], 'sdk_commit': spec['sdk_commit'],
        'rpcs3': json.loads((P / 'packaging/rpcs3-windows.json').read_text()),
        'codecs': {p.name: sha(p) for p in (P / 'firmware/dev_flash/sys/external').glob('*.pic')},
        'game_libs': [], 'sevenzip_sha256': sha(S / 'usr/bin/7z.exe'), 'pgo': pgo}
(S / 'app/package.json').write_text(json.dumps(meta, indent=2))
for n in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
    copy(P / n, S / 'usr/share/licenses/port' / n)

# Start the launcher with the toolchain's Python and GTK, without a console window.
bindir = '%~dp0usr\\toolchain\\ucrt64\\bin' if args.bundle else str(tools)
(S / 'BattrideWarRecomp.cmd').write_bytes((
    '@echo off\r\n'
    'set "PORT_APPDIR=%~dp0."\r\n'
    'set "PATH={0};%~dp0usr\\bin;%PATH%"\r\n'
    'start "" "{0}\\pythonw.exe" "%~dp0app\\launcher.py" %*\r\n').format(bindir).encode())
print('Package:', S, '(%d disc files in the manifest)' % len(manifest))
