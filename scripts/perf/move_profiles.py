#!/usr/bin/env python3
"""Rename GCC profiles trained before the port's sources moved into port/.

A profile is named after its object's path below the build directory
(CMakeFiles#kamenriderbattridewarsport.dir#stubs.cpp.gcda); since November
2026 the sources are in port/<area>/, so the same object is
...dir#port#core#stubs.cpp.gcda. The contents do not depend on the path, so
renaming keeps profiles from scripts/perf/pgo_train.py usable without training
again. Files already renamed, and the translated code's, are left alone.

usage: move_profiles.py <profile directory>
"""
import sys
from pathlib import Path

AREAS = {
    'core': ('main.cpp', 'stubs.cpp', 'loader_bridge.cpp', 'ppu_native.cpp'),
    'io': ('fs_bridge.cpp',),
    'audio': ('sail_bridge.cpp',),
}
TARGET = 'CMakeFiles#kamenriderbattridewarsport.dir#'


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    folder = Path(sys.argv[1])
    moved = 0
    for area, names in AREAS.items():
        for name in names:
            old = folder / (TARGET + name + '.gcda')
            if old.is_file():
                old.rename(folder / ('%sport#%s#%s.gcda' % (TARGET, area, name)))
                moved += 1
    print('Renamed %d profiles in %s' % (moved, folder))


if __name__ == '__main__':
    main()
