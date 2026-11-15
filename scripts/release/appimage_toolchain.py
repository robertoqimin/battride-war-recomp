#!/usr/bin/env python3
"""Assemble the relocatable Ubuntu 22.04 compiler toolchain for the AppImage.

The AppImage builds the game on the player's computer from their own disc.
This copies the build image's compilers, binutils, CMake, Ninja, headers and
static shader libraries into a sysroot. Compilers run with --sysroot, so the
result links against glibc 2.35 whatever the host has. Tool processes find
their own non-system libraries through runtime/; the host supplies libc and
the C++ runtime, exactly as for the rest of the AppImage.
"""
import os
import re
from pathlib import Path
import shutil
import subprocess

PACKAGES = (
    'gcc-11', 'g++-11', 'cpp-11', 'libgcc-11-dev', 'libstdc++-11-dev', 'libc6-dev', 'libc6',
    'linux-libc-dev', 'libcrypt-dev', 'libcrypt1', 'libgcc-s1', 'libstdc++6',
    'binutils', 'binutils-x86-64-linux-gnu', 'binutils-common', 'libbinutils', 'libctf0', 'libctf-nobfd0',
    'clang-14', 'libclang-common-14-dev', 'libclang-cpp14', 'libllvm14',
    'cmake', 'cmake-data', 'ninja-build', 'patch', 'patchelf', 'pkg-config',
    'libsdl2-dev', 'libsdl2-2.0-0', 'libvulkan-dev', 'glslang-dev', 'spirv-tools',
)
SKIP = ('usr/share/doc/', 'usr/share/man/', 'usr/share/info/', 'usr/share/locale/', 'usr/share/lintian/',
        'usr/share/bash-completion/', 'usr/share/emacs/', 'usr/share/vim/', 'usr/share/aclocal/',
        'usr/share/cmake-3.22/Help/', 'usr/lib/llvm-14/lib/clang/14.0.0/lib/',
        'usr/lib/llvm-14/share/', 'usr/share/gdb/')
# Executables the build runs; their library closure goes to runtime/.
TOOLS = ('usr/bin/x86_64-linux-gnu-gcc-11', 'usr/bin/x86_64-linux-gnu-g++-11', 'usr/lib/llvm-14/bin/clang',
         'usr/lib/gcc/x86_64-linux-gnu/11/cc1', 'usr/lib/gcc/x86_64-linux-gnu/11/cc1plus',
         'usr/lib/gcc/x86_64-linux-gnu/11/collect2', 'usr/lib/gcc/x86_64-linux-gnu/11/lto-wrapper',
         'usr/lib/gcc/x86_64-linux-gnu/11/liblto_plugin.so', 'usr/bin/x86_64-linux-gnu-as',
         'usr/bin/x86_64-linux-gnu-ld.bfd', 'usr/bin/x86_64-linux-gnu-ar', 'usr/bin/x86_64-linux-gnu-ranlib',
         'usr/bin/cmake', 'usr/bin/ninja', 'usr/bin/patch', 'usr/bin/patchelf', 'usr/bin/x86_64-linux-gnu-pkg-config')
# Only glibc and the C++ runtime come from the host for tool processes; the
# tools' other libraries (LLVM 14 included, under its own soname) are private.
SYSTEM = ('ld-linux', 'libc.so', 'libm.so', 'libpthread.so', 'libdl.so', 'librt.so', 'libresolv.so',
          'libutil.so', 'libanl.so', 'libnss_', 'libstdc++', 'libgcc_s')
# Linked into the game; their shared-library closure must exist for ld.
LINKED = ('usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0',)


# System files copied into the sysroot, for the corresponding-source collection.
COPIED = set()


def placed(root, path):
    """Path inside root for an absolute path, following merged-/usr directories."""
    parent = os.path.realpath(os.path.dirname(path))
    return root / parent.lstrip('/') / os.path.basename(path)


def copy_entry(root, path):
    dest = placed(root, path)
    if dest.exists() or dest.is_symlink():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    if os.path.islink(path):
        target = os.readlink(path)
        if os.path.isabs(target):
            target = os.path.relpath(placed(root, target), dest.parent)
        dest.symlink_to(target)
        copy_entry(root, os.path.join(os.path.dirname(path), os.readlink(path)))
    elif os.path.isfile(path):
        shutil.copy2(path, dest)
        COPIED.add(path)


def closure(paths):
    found = set()
    for p in paths:
        out = subprocess.run(['ldd', p], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split('=>')
            if len(parts) == 2 and parts[1].split() and parts[1].split()[0].startswith('/'):
                found.add(parts[1].split()[0])
    return found


def assemble(dest):
    dest = Path(dest)
    root = dest / 'sysroot'
    root.mkdir(parents=True)
    for d in ('lib', 'lib64', 'bin', 'sbin'):
        (root / 'usr' / d).mkdir(parents=True, exist_ok=True)
        (root / d).symlink_to('usr/' + d)
    listed = subprocess.check_output(['dpkg', '-L', *PACKAGES], text=True).splitlines()
    for path in listed:
        rel = os.path.realpath(path).lstrip('/')
        if not path.startswith('/') or os.path.isdir(path) and not os.path.islink(path):
            continue
        if rel.startswith(SKIP) or (rel.startswith('usr/bin/spirv-') or rel.startswith('usr/bin/glslang')):
            continue
        copy_entry(root, path)
    tools = [str(Path('/') / t) for t in TOOLS]
    for lib in closure([*tools, *('/' + p for p in LINKED)]):
        copy_entry(root, lib)
    runtime = dest / 'runtime'
    runtime.mkdir()
    for lib in sorted(closure(tools)):
        name = os.path.basename(lib)
        if not name.startswith(SYSTEM):
            (runtime / name).symlink_to(os.path.relpath(placed(root, lib), runtime))
    # Tools find runtime/ through a relative DT_RPATH (inherited by the libraries
    # they load), never LD_LIBRARY_PATH: that would leak these libraries into the
    # host programs the build starts, such as /bin/sh, which on Arch is bash
    # linked against the host's newer readline.
    for tool in TOOLS:
        path = os.path.realpath(placed(root, '/' + tool))
        if not os.path.isfile(path):
            continue
        with open(path, 'rb') as f:
            if f.read(4) != b'\x7fELF':
                continue  # e.g. the pkg-config wrapper script
        rpath = '$ORIGIN/' + os.path.relpath(runtime, os.path.dirname(path))
        subprocess.run(['patchelf', '--force-rpath', '--set-rpath', rpath, path], check=True)
    # A library with its own RUNPATH (LLVM's $ORIGIN/../lib) does not see the
    # executable's RPATH for its dependencies; point it at runtime/ too.
    for link in runtime.iterdir():
        path = os.path.realpath(link)
        if 'RUNPATH' in subprocess.run(['readelf', '-d', path], capture_output=True, text=True).stdout:
            rpath = '$ORIGIN/' + os.path.relpath(runtime, os.path.dirname(path))
            subprocess.run(['patchelf', '--force-rpath', '--set-rpath', rpath, path], check=True)
    # Some Ubuntu package configs hardcode /usr (SDL2's prefix, SPIRV-Tools'
    # librt.a). Inside the sysroot they must follow CMAKE_SYSROOT, or the game
    # would compile against host headers or fail to find libraries.
    for config in (root / 'usr/lib/x86_64-linux-gnu/cmake').rglob('*.cmake'):
        text = config.read_text()
        fixed = re.sub(r'(?<=["; ])/usr/(lib|include)/', r'${CMAKE_SYSROOT}/usr/\1/', text)
        fixed = fixed.replace('set(prefix "/usr")', 'set(prefix "${CMAKE_SYSROOT}/usr")')
        if fixed != text:
            config.write_text(fixed)
    if '${CMAKE_SYSROOT}' not in (root / 'usr/lib/x86_64-linux-gnu/cmake/SDL2/sdl2-config.cmake').read_text():
        raise SystemExit('Unexpected sdl2-config.cmake layout')
    (dest / 'toolchain.cmake').write_text(TOOLCHAIN)
    return dest


TOOLCHAIN = r'''# Relocatable toolchain for building the game inside the AppImage.
set(_root "${CMAKE_CURRENT_LIST_DIR}/sysroot")
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT "${_root}")
set(CMAKE_C_COMPILER "${_root}/usr/lib/llvm-14/bin/clang")
set(CMAKE_CXX_COMPILER "${_root}/usr/bin/x86_64-linux-gnu-g++-11")
set(CMAKE_AR "${_root}/usr/bin/x86_64-linux-gnu-ar")
set(CMAKE_RANLIB "${_root}/usr/bin/x86_64-linux-gnu-ranlib")
set(CMAKE_LINKER "${_root}/usr/bin/x86_64-linux-gnu-ld.bfd")
set(CMAKE_MAKE_PROGRAM "${_root}/usr/bin/ninja" CACHE FILEPATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH "${_root}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_root}")
set(ENV{PKG_CONFIG_LIBDIR} "${_root}/usr/lib/x86_64-linux-gnu/pkgconfig:${_root}/usr/share/pkgconfig")
'''
