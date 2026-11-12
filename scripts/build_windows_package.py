#!/usr/bin/env python3
"""Assemble the Windows package the launcher runs from (packaging/launcher.py).

The same layout as the AppImage (app/, usr/bin, usr/share/battride), with
Windows tools: 7-Zip, FFmpeg and graphics-probe.exe. The compiler toolchain is
not copied yet: usr/toolchain/toolchain.json points at an MSYS2 UCRT64
installation and at a Python with the SDK tools' requirements, so this package
runs on the computer that built it (a self-contained one is stage 6 of
docs/WINDOWS.md). Like the AppImage it holds no game code or assets: the disc
manifest has only names, sizes and SHA-256 of the files the import verifies.

usage: build_windows_package.py --stage <new dir> --sevenzip-dir <dir with 7z.exe and 7z.dll>
       --ffmpeg-dir <dir with ffmpeg.exe and ffprobe.exe> --python <python with tools/requirements.txt>
       [--toolchain C:/msys64/ucrt64] [--sdk ../ps3recomp]
Run it with the MSYS2 UCRT64 tools on PATH (graphics-probe.exe is built here).
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

P = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--stage', type=Path, required=True)
ap.add_argument('--sevenzip-dir', type=Path, required=True)
ap.add_argument('--ffmpeg-dir', type=Path, required=True)
ap.add_argument('--python', type=Path, required=True)
ap.add_argument('--toolchain', type=Path, default=Path('C:/msys64/ucrt64'))
ap.add_argument('--sdk', type=Path, default=P.parent / 'ps3recomp')
args = ap.parse_args()
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

# The toolchain the launcher builds the game with (this computer's MSYS2 for now).
(S / 'usr/toolchain').mkdir(parents=True)
(S / 'usr/toolchain/toolchain.json').write_text(json.dumps({'bin': str(tools), 'python': str(args.python.resolve())},
                                                           indent=2) + '\n')

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
        'game_libs': [], 'sevenzip_sha256': sha(S / 'usr/bin/7z.exe'), 'pgo': False}
(S / 'app/package.json').write_text(json.dumps(meta, indent=2))
for n in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
    copy(P / n, S / 'usr/share/licenses/port' / n)

# Start the launcher with the toolchain's Python and GTK, without a console window.
(S / 'BattrideWarRecomp.cmd').write_bytes((
    '@echo off\r\n'
    'set "PORT_APPDIR=%~dp0."\r\n'
    'set "PATH={0};%~dp0usr\\bin;%PATH%"\r\n'
    'start "" "{0}\\pythonw.exe" "%~dp0app\\launcher.py" %*\r\n').format(tools).encode())
print('Package:', S, '(%d disc files in the manifest)' % len(manifest))
