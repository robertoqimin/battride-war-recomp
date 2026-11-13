#!/usr/bin/env python3
"""Collect the corresponding source code for every binary in the Windows package.

The Windows counterpart of collect_sources.py, run after
build_windows_package.py --bundle with network access. Every bundled binary
comes from an MSYS2 UCRT64 package listed with its exact version and source
package in usr/toolchain/packages.json (the DLLs beside graphics-probe.exe and
the game come from the same packages); MSYS2 publishes each source package,
with its PKGBUILD, patches and upstream tarballs, at repo.msys2.org. The
7-Zip source is added for usr/bin/7z.exe. The result is one tar for the release
page with a SOURCES.md index.

usage: collect_windows_sources.py --package <stage> --out <file.tar> [--check]
       --check only asks the servers that every source exists and reports the total size.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import urllib.request

MSYS2_SOURCES = 'https://repo.msys2.org/mingw/sources/'
SEVENZIP_SOURCES = 'https://github.com/ip7z/7zip/releases/download/%s.%s/'   # the 7-Zip author's releases


def head(url):
    with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=60) as r:
        return int(r.headers.get('Content-Length') or 0)


def fetch(url, dest):
    h = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=300) as r, open(dest, 'wb') as f:
        for block in iter(lambda: r.read(1 << 20), b''):
            f.write(block)
            h.update(block)
    return h.hexdigest()


def sources(package):
    """(file name, URL, what it covers) for every bundled binary."""
    listed = json.loads((package / 'usr/toolchain/packages.json').read_text())
    by_base = {}
    for p in listed:
        by_base.setdefault((p['base'], p['version']), []).append(p['name'])
    out = [('%s-%s.src.tar.zst' % key, MSYS2_SOURCES + '%s-%s.src.tar.zst' % key, ', '.join(sorted(names)))
           for key, names in sorted(by_base.items())]
    banner = subprocess.run([str(package / 'usr/bin/7z.exe')], capture_output=True, text=True).stdout
    version = re.search(r'7-Zip (\d+)\.(\d+)', banner)
    if not version:
        raise SystemExit('Cannot read the 7-Zip version from usr/bin/7z.exe')
    name = '7z%s%s-src.tar.xz' % version.groups()
    out.append((name, SEVENZIP_SOURCES % version.groups() + name, 'usr/bin/7z.exe, usr/bin/7z.dll (7-Zip %s.%s)' % version.groups()))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--package', type=Path, required=True)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    items = sources(args.package)
    if args.check:
        total, missing = 0, []
        for name, url, _ in items:
            try:
                total += head(url)
            except OSError as e:
                missing.append('%s (%s)' % (url, e))
        print('%d source archives, %.0f MB' % (len(items), total / 1e6))
        if missing:
            raise SystemExit('Not found:\n' + '\n'.join(missing))
        return
    if not args.out:
        ap.error('--out is required unless --check')
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(args.out, 'w') as tar:
        index = ['# Sources of the Windows package', '',
                 'Each archive is the complete source of the binaries named beside it, at the exact '
                 'version in the package (usr/toolchain/packages.json).', '',
                 '| Archive | SHA-256 | Covers |', '|---|---|---|']
        for name, url, covers in items:
            print('Downloading', url, flush=True)
            path = Path(tmp) / name
            digest = fetch(url, path)
            tar.add(path, 'sources/' + name)
            path.unlink()
            index.append('| %s | `%s` | %s |' % (name, digest, covers))
        md = Path(tmp) / 'SOURCES.md'
        md.write_text('\n'.join(index) + '\n')
        tar.add(md, 'SOURCES.md')
    print('Wrote', args.out)


if __name__ == '__main__':
    main()
