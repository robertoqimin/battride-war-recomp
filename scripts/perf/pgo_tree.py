"""Prepare a PGO training tree laid out as the importer's build: <root>/port and,
next to it, the build directory <root>/build.

usage: pgo_tree.py --stage <package stage> --root <new folder> [--eboot game/EBOOT.ELF]

PGO builds compile with this tree's paths relative to the build directory
(scripts/build/pgo_relpaths.py), so profiles fit the player's build only if
training saw the same relative layout: the importer copies the package's
usr/share/battride/port to <work>/port, translates the EBOOT there with the
package's SDK and builds in <work>/build. This does the first two steps from
a package stage (build-appimage/<v>/AppDir on Linux, the Windows package's
stage on Windows); then configure <root>/build as the importer does with
-DPORT_PGO=generate (docs/APPIMAGE.md, "Optimization profiles") and run
scripts/perf/pgo_train.py --build <root>/build.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stage', type=Path, required=True)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--eboot', type=Path, default=REPO / 'game/EBOOT.ELF')
    args = ap.parse_args()
    share = args.stage.resolve() / 'usr/share/battride'
    if not (share / 'port/CMakeLists.txt').is_file():
        sys.exit('Not a package stage (no usr/share/battride/port): %s' % args.stage)
    root = args.root.resolve()
    if root.exists():
        sys.exit('Choose a new folder; %s exists.' % root)
    port = root / 'port'
    shutil.copytree(share / 'port', port)
    (port / 'game').mkdir(exist_ok=True)
    shutil.copy2(args.eboot, port / 'game/EBOOT.ELF')
    print('Translating the EBOOT in %s ...' % port, flush=True)
    code = subprocess.call([sys.executable, str(port / 'scripts/build/generate.py'),
                            '--sdk', str(share / 'ps3recomp'), '--jobs', str(os.cpu_count() or 2)], cwd=port)
    if code:
        sys.exit('generate.py failed (%d)' % code)
    print('Ready. Configure %s with -DPS3RECOMP_DIR=%s -DPORT_PGO=generate as the importer does.'
          % (root / 'build', share / 'ps3recomp'))


if __name__ == '__main__':
    main()
