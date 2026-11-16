"""Compiler launcher for PGO builds: run the compiler with this tree's paths
relative to the build directory.

usage (CMAKE_CXX_COMPILER_LAUNCHER): pgo_relpaths.py <source dir> <compiler> <args...>

GCC 11 hashes each function's source file name, as the compiler saw it, into
the checksums and ids its profiles are matched by. With CMake's absolute paths
the names hold the training machine's folder, and in the player's build (a
fresh folder, another user's home) the profiles of the translated code only
partly apply: the same chunk compiled elsewhere came out with a different
layout and about 1 fps less in a fight. Relative to the build directory, the
names are the same wherever the tree is, as long as the build directory sits
in the same place relative to it: the importer's work/port and work/build,
which training must copy (docs/APPIMAGE.md, "Optimization profiles").
"""
import os
import subprocess
import sys


def main():
    source = os.path.abspath(sys.argv[1]).replace('\\', '/').rstrip('/') + '/'
    relative = os.path.relpath(source, os.getcwd()).replace('\\', '/').rstrip('/') + '/'
    args = []
    for arg in sys.argv[2:]:
        plain = arg.replace('\\', '/')
        i = plain.find(source)
        if i < 0 and os.name == 'nt':
            i = plain.lower().find(source.lower())
        args.append(plain[:i] + relative + plain[i + len(source):] if i >= 0 else arg)
    # The compiler itself keeps its own path.
    args[0] = sys.argv[2]
    return subprocess.call(args)


if __name__ == '__main__':
    sys.exit(main())
