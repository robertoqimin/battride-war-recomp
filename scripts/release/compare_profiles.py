#!/usr/bin/env python3
"""Compare the GCC profiles of two packages before a release.

usage: compare_profiles.py <previous> <new>

Each argument is a profile directory (with profile.json), a package stage
(build-appimage/<v>/AppDir or the Windows package folder) or an extracted
AppImage (squashfs-root); the profiles are taken from usr/share/battride/pgo.

The importer rebuilds the player's game when the shipped profiles change
(build_identity() records their hash), so a release should ship the previous
profiles byte for byte unless the translated code changed: the lifter, the
headers the PPU chunks include, their compile flags or the GCC version
(docs/PLANO0.6.1.md, U4). Exit status 0 when both sets are identical, 1 when
they differ (the differences are listed), 2 on a usage error.
"""
import hashlib
import json
from pathlib import Path
import sys


def profile_dir(path):
    path = Path(path)
    for candidate in (path, path / 'usr/share/battride/pgo'):
        if (candidate / 'profile.json').is_file():
            return candidate
    raise SystemExit('No profiles (profile.json) in %s or its usr/share/battride/pgo' % path)


def files(path):
    out = {}
    for p in sorted(path.rglob('*')):
        if p.is_file():
            out[p.relative_to(path).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    old_dir, new_dir = profile_dir(sys.argv[1]), profile_dir(sys.argv[2])
    old, new = files(old_dir), files(new_dir)
    old_info = json.loads((old_dir / 'profile.json').read_text())
    new_info = json.loads((new_dir / 'profile.json').read_text())
    changed = sorted(n for n in old.keys() & new.keys() if old[n] != new[n])
    removed, added = sorted(old.keys() - new.keys()), sorted(new.keys() - old.keys())
    if not (changed or removed or added):
        print('Identical: %d files. Players keep the game they built (if nothing else changed).' % len(new))
        return 0
    print('The profiles differ: every player rebuilds the game (15-60 minutes).')
    for key in ('gcc', 'sdk_commit', 'eboot_sha256', 'sessions', 'files'):
        if old_info.get(key) != new_info.get(key):
            print('  profile.json %s: %s -> %s' % (key, old_info.get(key), new_info.get(key)))
    for label, names in (('changed', changed), ('removed', removed), ('added', added)):
        if names:
            print('  %d %s: %s%s' % (len(names), label, ', '.join(names[:8]), ' ...' if len(names) > 8 else ''))
    print('Ship the previous profiles unless the translated code, its flags or GCC changed.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
