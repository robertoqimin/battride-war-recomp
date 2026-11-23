#!/usr/bin/env python3
"""Build the public AppImage inside the documented Ubuntu 22.04 build image.

The AppImage carries no game code or assets (only optional GCC profiles,
execution counts of the translated code). It ships the port's public sources,
the pinned ps3recomp SDK and a compiler toolchain; the launcher translates the
player's own EBOOT and compiles the game on their computer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import sysconfig
import tarfile
import io

sys.path.insert(0,str(Path(__file__).resolve().parent))
import appimage_toolchain

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--sevenzip-dir',type=Path,required=True)
ap.add_argument('--runtime',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--stage',type=Path,default=Path('build-appimage/portable/AppDir'))
ap.add_argument('--sdk',type=Path,help='ps3recomp git checkout (default: ../ps3recomp)')
ap.add_argument('--pgo',type=Path,help='GCC profiles trained with scripts/perf/pgo_train.py on a build with this toolchain')
args=ap.parse_args()
P=Path(__file__).resolve().parents[2];S=args.stage.resolve()
if S.exists() or args.output.exists():raise SystemExit('Choose new stage and output paths; existing files are preserved.')
# Never package a newer build as if it had the documented compatibility floor.
if subprocess.check_output(['getconf','GNU_LIBC_VERSION'],text=True).strip()!='glibc 2.35':
    raise SystemExit('Build inside the Ubuntu 22.04 container (glibc 2.35).')

# Every file taken from the build image, so collect_sources.py can find its package.
system_files=set()
def copy(a,b):
    b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a,b)
    if str(a).startswith(('/usr/','/lib','/etc/')):system_files.add(os.path.realpath(a))
def copytree(a,b,**kw):
    shutil.copytree(a,b,**kw)
    system_files.update(str(p) for p in Path(a).rglob('*') if p.is_file() and not p.is_symlink() and (b/p.relative_to(a)).exists())
def sha(p):
    h=hashlib.sha256()
    with open(p,'rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()

# These libraries must match the host's GPU driver. Never ship a second libc,
# dynamic loader, C++ runtime, Vulkan/GL loader, DRM or LLVM/SPIRV implementation.
# The X11/xcb/Wayland entries are the ones Mesa and NVIDIA's GLX/EGL link; other
# toolkit helpers (GLEW, GLU, xcb-util, xkbcommon) are not on every host and ship.
def host_library(name):
    return (name.startswith(('ld-linux','libc.so','libm.so','libpthread.so','libdl.so','librt.so',
            'libresolv.so','libnss_','libutil.so','libanl.so','libstdc++','libgcc_s','libatomic.so',
            'libvulkan','libGL.so','libGLX','libGLdispatch','libglapi','libEGL','libOpenGL',
            'libdrm','libgbm','libLLVM','libSPIRV','libudev','libexpat','libelf','libz.so','libzstd',
            'libX11.so','libX11-xcb','libXau','libXdmcp','libXext','libXfixes','libXxf86vm','libxshmfence',
            'libxcb.so','libxcb-dri2','libxcb-dri3','libxcb-present','libxcb-sync','libxcb-xfixes',
            'libxcb-randr','libxcb-shm','libxcb-glx','libwayland-client','libwayland-server','libwayland-egl')))

for d in ('app','usr/bin','usr/lib','usr/share/glib-2.0/schemas'):(S/d).mkdir(parents=True,exist_ok=True)
for n in ('importer.py','launcher.py'):copy(P/'packaging'/n,S/'app'/n)
copy(P/'CREDITS.md',S/'app/CREDITS.md')
spec=json.loads((P/'recompile.json').read_text())
# The base edition (BLJS10220): its disc list ships whole, the others as deltas;
# the shipped profiles are trained on it (docs/PLANO0.6.1.md, E4).
edition=spec['editions'][0]
# The port's public sources, exactly as the source export allowlist names them.
public=[l.strip() for l in (P/'PUBLIC_FILES.txt').read_text().splitlines() if l.strip() and not l.startswith('#')]
for n in public:
    if not (P/n).is_file() or (P/n).is_symlink():raise SystemExit('Public file missing: '+n)
    copy(P/n,S/'usr/share/battride/port'/n)
# The pinned SDK commit itself, not the working tree: local changes never ship.
sdk=(args.sdk or P.parent/'ps3recomp').resolve()
archive=subprocess.run(['git','-C',str(sdk),'archive','--format=tar',spec['sdk_commit']],check=True,capture_output=True).stdout
with tarfile.open(fileobj=io.BytesIO(archive)) as tar:tar.extractall(S/'usr/share/battride/ps3recomp')
(S/'usr/share/battride/ps3recomp/.ps3recomp-commit').write_text(spec['sdk_commit']+'\n')
# The port's own fixes to that commit (patches/ps3recomp/), in order. The
# ceiling keeps git from treating the export as part of an enclosing checkout.
for patch in sorted((P/'patches/ps3recomp').glob('*.patch')):
    subprocess.run(['git','apply',str(patch)],cwd=S/'usr/share/battride/ps3recomp',check=True,
                   env={**os.environ,'GIT_CEILING_DIRECTORIES':str((S/'usr/share/battride').resolve())})
print('Assembling the compiler toolchain...',flush=True)
appimage_toolchain.assemble(S/'usr/toolchain')
# Optional GCC profiles: execution counts of the translated code, which the
# player's build applies (PORT_PGO=use). They only fit the compiler, SDK and
# EBOOT they were trained with.
if args.pgo:
    info=json.loads((args.pgo/'profile.json').read_text())
    gcc=subprocess.check_output([str(S/'usr/toolchain/sysroot/usr/bin/x86_64-linux-gnu-g++-11'),'-dumpfullversion'],text=True).strip()
    if (info['gcc'],info['sdk_commit'],info['eboot_sha256'])!=(gcc,spec['sdk_commit'],edition['eboot_sha256']):
        raise SystemExit('The profiles in %s were trained with GCC %s, SDK %s; this build uses GCC %s, SDK %s.'
                         %(args.pgo,info['gcc'],info['sdk_commit'],gcc,spec['sdk_commit']))
    files=sorted(args.pgo.rglob('*.gcda'))
    if len(files)!=info['files']:raise SystemExit('profile.json lists %d profiles, found %d'%(info['files'],len(files)))
    # The SDK sources compiled into the game keep the training machine's SDK
    # path in their profile names. Ship them under a neutral path instead of
    # the developer's home directory; the importer renames from sdk_dir to the
    # player's SDK copy (importer.place_profiles).
    trained=info.get('sdk_dir','').replace('/','#')
    neutral='/ps3recomp'
    for f in files:
        name=str(f.relative_to(args.pgo))
        if trained:name=name.replace(trained,neutral.replace('/','#'))
        copy(f,S/'usr/share/battride/pgo'/name)
    (S/'usr/share/battride/pgo/profile.json').write_text(json.dumps(dict(info,sdk_dir=neutral),indent=2)+'\n')
    if any('home#' in n.name for n in (S/'usr/share/battride/pgo').iterdir()):
        raise SystemExit('A profile name still carries a home directory.')
copy(sys.executable,S/'usr/bin/python3')
version='%d.%d'%sys.version_info[:2]
copytree(sysconfig.get_path('stdlib'),S/'usr/lib'/('python'+version),ignore=shutil.ignore_patterns('site-packages','dist-packages','__pycache__','test','tests','idlelib','ensurepip','venv'))
for n in ('gi','cairo'):
    copyfrom=next((Path(p)/n for p in sys.path if (Path(p)/n).is_dir()))
    copytree(copyfrom,S/'usr/lib'/('python'+version)/'site-packages'/n,ignore=shutil.ignore_patterns('__pycache__'))
# GTK resources are private; application GIO plugins from the host are disabled.
copytree('/usr/lib/x86_64-linux-gnu/girepository-1.0',S/'usr/lib/girepository-1.0')
copy('/usr/share/glib-2.0/schemas/gschemas.compiled',S/'usr/share/glib-2.0/schemas/gschemas.compiled')
# GTK draws the symbolic icons from SVG through gdk-pixbuf's librsvg loader. The
# loader cache needs absolute paths, so the launcher fills in this AppImage's mount.
pixbuf=Path('/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0')
copy(pixbuf/'loaders/libpixbufloader-svg.so',S/'usr/lib/gdk-pixbuf/libpixbufloader-svg.so')
entry=next(b for b in (pixbuf/'loaders.cache').read_text().split('\n\n') if 'libpixbufloader-svg.so' in b)
(S/'usr/lib/gdk-pixbuf/loaders.cache.in').write_text(entry.replace(str(pixbuf/'loaders'),'@LOADERS@').strip()+'\n\n')
copy(args.sevenzip_dir/'7zz',S/'usr/bin/7z');copy(args.sevenzip_dir/'License.txt',S/'usr/share/licenses/7zip/License.txt')
for n in ('ffmpeg','ffprobe'):copy('/usr/bin/'+n,S/'usr/bin'/n)
subprocess.run(['cc','-O2','-march=x86-64','-msse4.1','-Wl,--disable-new-dtags,-rpath,$ORIGIN/../lib',str(P/'packaging/graphics_probe.c'),'-o',str(S/'usr/bin/graphics-probe'),*subprocess.check_output(['pkg-config','--cflags','--libs','sdl2','vulkan'],text=True).split()],check=True)
seeds=[]
libdir=Path('/usr/lib/x86_64-linux-gnu')
for pattern in ('libgtk-4.so.1','libadwaita-1.so.0','libgirepository-1.0.so.1','libpulse.so.0','libasound.so.2','libdecor-0.so.0'):
    seeds.extend(libdir.glob(pattern))
# The toolchain resolves its own libraries (runtime/); the SDK holds a PPU test ELF.
skip=(str(S/'usr/toolchain'),str(S/'usr/share/battride'))
queue=seeds+[p for p in S.rglob('*') if p.is_file() and not p.is_symlink() and not str(p).startswith(skip)]
seen=set();bom={};missing=set()
while queue:
    p=queue.pop();real=p.resolve()
    if real in seen:continue
    seen.add(real)
    try:
        with p.open('rb') as f:
            if f.read(4)!=b'\x7fELF':continue
    except OSError:continue
    if not str(p).startswith(str(S)):
        if host_library(p.name):continue
        dest=S/'usr/lib'/p.name
        if not dest.exists():copy(p,dest);bom[p.name]=str(real)
    out=subprocess.run(['ldd',str(p)],capture_output=True,text=True).stdout
    for line in out.splitlines():
        match=re.search(r'=> (/\S+)',line) or re.match(r'\s*(/\S+)',line)
        if match and Path(match[1]).exists():queue.append(Path(match[1]))
        elif 'not found' in line:missing.add(line.strip())
# No LD_LIBRARY_PATH: it would leak these libraries into host programs started
# from here (on Arch, /bin/sh is bash linked against a newer readline). Bundled
# executables carry a relative DT_RPATH, which the libraries they load inherit.
for n in ('python3','ffmpeg','ffprobe'):
    subprocess.run(['patchelf','--force-rpath','--set-rpath','$ORIGIN/../lib',str(S/'usr/bin'/n)],check=True)
# A bundled library with its own RUNPATH (libpulse points at Ubuntu's
# pulseaudio directory) would not see that RPATH; its dependencies sit beside it.
for p in (S/'usr/lib').iterdir():
    if p.is_file() and not p.is_symlink() and '.so' in p.name and 'RUNPATH' in subprocess.run(['readelf','-d',str(p)],capture_output=True,text=True).stdout:
        subprocess.run(['patchelf','--force-rpath','--set-rpath','$ORIGIN',str(p)],check=True)
# SDL2 and the libraries it needs from this AppImage; the launcher copies them
# next to the game it builds, which finds them through its own $ORIGIN RPATH.
out=subprocess.run(['ldd',str(S/'usr/lib/libSDL2-2.0.so.0')],capture_output=True,text=True,env=dict(os.environ,LD_LIBRARY_PATH=str(S/'usr/lib'))).stdout
game_libs=['libSDL2-2.0.so.0']+sorted({Path(m[1]).name for m in re.finditer(r'=> (\S+)',out) if m[1].startswith(str(S/'usr/lib'))})
# Record unresolved imports for review; none are expected.
(S/'app/unresolved-build-libraries.txt').write_text('\n'.join(sorted(missing)))
manifest={}
for p in sorted((P/'game/PS3_GAME').rglob('*')):
    if p.is_file() and p.suffix.lower()!='.elf':manifest[str(p.relative_to(P/'game'))]={'size':p.stat().st_size,'sha256':sha(p)}
p=P/'game/PS3_DISC.SFB'
if p.exists():manifest['PS3_DISC.SFB']={'size':p.stat().st_size,'sha256':sha(p)}
(S/'app/disc-manifest.json').write_text(json.dumps(manifest,separators=(',',':')))
# The other editions' discs, as deltas against this one (packaging/discs, made by
# scripts/release/disc_delta.py); the importer rebuilds their full lists (importer.apply_delta).
editions = [{'title_id': e['title_id'], 'name': e['name'], 'elf_sha256': e['eboot_sha256']} for e in spec['editions']]
base_sha256 = hashlib.sha256(json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
(S / 'app/discs').mkdir(exist_ok=True)
for e in editions[1:]:
    delta = P / 'packaging/discs' / (e['title_id'] + '.json')
    if json.loads(delta.read_text())['base_sha256'] != base_sha256:
        raise SystemExit('%s was made from another %s disc list; run scripts/release/disc_delta.py again.'
                         % (delta, editions[0]['title_id']))
    shutil.copy2(delta, S / 'app/discs' / delta.name)
    e['delta'] = 'discs/' + delta.name
meta={'version':'0.6.0','glibc_min':'2.35','cpu':'built on the player computer, -march=native','elf_sha256':edition['eboot_sha256'],
      'sdk_commit':spec['sdk_commit'],'rpcs3':json.loads((P/'packaging/linux/rpcs3.json').read_text()),'codecs':{p.name:sha(p) for p in (P/'firmware/dev_flash/sys/external').glob('*.pic')},
      'game_libs':game_libs,'sevenzip_sha256':sha(S/'usr/bin/7z'),'runtime_sha256':sha(args.runtime),'pgo':args.pgo is not None,'editions':editions}
(S/'app/package.json').write_text(json.dumps(meta,indent=2));(S/'app/library-sources.json').write_text(json.dumps(bom,indent=2))
# Exact Ubuntu package versions in the build image, for the corresponding sources.
system_files.update(appimage_toolchain.COPIED)
(S/'app/system-files.txt').write_text('\n'.join(sorted(system_files))+'\n')
(S/'app/build-packages.txt').write_text(subprocess.check_output(['dpkg-query','-W','-f=${Package} ${Version}\\n'],text=True))
(S/'AppRun').write_text('''#!/bin/sh
APPDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PORT_APPDIR="$APPDIR" PYTHONHOME="$APPDIR/usr" PYTHONPLATLIBDIR=lib
export PYTHONPATH="$APPDIR/app:$APPDIR/usr/lib/python'''+version+'''/site-packages" PYTHONDONTWRITEBYTECODE=1
export GI_TYPELIB_PATH="$APPDIR/usr/lib/girepository-1.0" GSETTINGS_SCHEMA_DIR="$APPDIR/usr/share/glib-2.0/schemas"
export GSETTINGS_BACKEND=memory GIO_USE_VFS=local GIO_MODULE_DIR="$APPDIR/usr/lib/gio/modules"
export PATH="$APPDIR/usr/bin:$PATH"
unset LD_LIBRARY_PATH LD_PRELOAD
ulimit -c 0
exec "$APPDIR/usr/bin/python3" "$APPDIR/app/launcher.py" "$@"
''');(S/'AppRun').chmod(0o755)
(S/'usr/lib/gio/modules').mkdir(parents=True,exist_ok=True)
(S/'battride-recomp.desktop').write_text('[Desktop Entry]\nType=Application\nName=Battride War Recomp\nExec=AppRun\nIcon=battride-recomp\nCategories=Game;\nTerminal=false\n')
(S/'battride-recomp.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128"><rect width="128" height="128" rx="24" fill="#14232d"/><text x="64" y="83" font-size="60" text-anchor="middle" fill="#38dfa0" font-family="sans-serif">BR</text></svg>')
(S/'.DirIcon').symlink_to('battride-recomp.svg')
for n in ('LICENSE','THIRD_PARTY_NOTICES.md'):copy(P/n,S/'usr/share/licenses/port'/n)
shutil.copytree('/usr/share/doc',S/'usr/share/licenses/build-packages',symlinks=True,ignore=shutil.ignore_patterns('examples','html'))
print('STAGED',S,flush=True)
args.output.parent.mkdir(parents=True,exist_ok=True);payload=args.output.with_suffix('.squashfs.tmp')
if payload.exists():raise SystemExit('Temporary payload already exists')
subprocess.run(['mksquashfs',str(S),str(payload),'-noappend','-all-root','-no-xattrs','-comp','zstd','-processors','2'],check=True)
with args.output.open('xb') as out,args.runtime.open('rb') as rt,payload.open('rb') as squash:
    shutil.copyfileobj(rt,out);shutil.copyfileobj(squash,out)
args.output.chmod(0o755);payload.unlink()
args.output.with_suffix('.AppImage.sha256').write_text(sha(args.output)+'  '+args.output.name+'\n')
print('AppImage:',args.output)
