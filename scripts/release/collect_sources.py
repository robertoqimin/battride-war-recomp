#!/usr/bin/env python3
"""Collect the corresponding source code for every binary component of the AppImage.

Run as root inside the same Ubuntu 22.04 build image right after
build_appimage.py; it needs network access. It maps each file the build copied
from the image (app/system-files.txt) to its Ubuntu package, downloads the
source packages of exactly the installed versions, adds the 7-Zip and AppImage
runtime sources, and writes one tar for the release page.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

# The AppImage type2 runtime and the libraries its build script compiles into
# it statically (scripts/common/install-dependencies.sh at that commit).
RUNTIME_DEPS = (
    ('fuse-3.15.0.tar.xz', 'https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz',
     '70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0'),
    ('squashfuse-0.5.2.tar.gz', 'https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz',
     'db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3'),
)


def fetch(url, dest, sha256=None):
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read()
    if sha256 and hashlib.sha256(data).hexdigest() != sha256:
        raise SystemExit('Checksum mismatch for ' + url)
    dest.write_bytes(data)


def reused_complete(ubuntu, src, version):
    """True when a reused download already holds src=version whole.

    The archive keeps only the newest version of a package (linux-libc-dev
    5.15.0-198.208 was superseded after 0.5.1), so apt-get source fails for
    the one in the image. A copy downloaded for an earlier release is the
    same source: accept it when its .dsc names that version and every file
    the .dsc lists is present with the SHA-256 it records."""
    dsc = ubuntu / f'{src}_{version.split(":", 1)[-1]}.dsc'
    if not dsc.is_file():
        return False
    text = dsc.read_text(errors='replace')
    if not any(l.startswith('Version:') and l.split(':', 1)[1].strip() == version for l in text.splitlines()):
        return False
    if '\nChecksums-Sha256:\n' not in text:
        return False
    entries = []
    for line in text.split('\nChecksums-Sha256:\n', 1)[1].splitlines():
        if not line.startswith(' '):
            break                       # the next field (Files:, with MD5 sums)
        entries.append(line.split())
    for sha, _size, name in entries:
        path = ubuntu / name
        if not path.is_file():
            return False
        h = hashlib.sha256()
        with path.open('rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        if h.hexdigest() != sha:
            return False
    print(f'{src}={version}: no longer in the archive; using the verified copy from --reuse', flush=True)
    return bool(entries)


def owners():
    """Map every packaged path, also under its merged-/usr real location, to its binary package."""
    table = {}
    for listing in Path('/var/lib/dpkg/info').glob('*.list'):
        package = listing.stem.split(':')[0]
        for line in listing.read_text(errors='replace').splitlines():
            if line:
                table.setdefault(line, package)
                real = os.path.join(os.path.realpath(os.path.dirname(line)), os.path.basename(line))
                table.setdefault(real, package)
    return table


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stage', type=Path, required=True, help='AppDir produced by build_appimage.py')
    ap.add_argument('--out', type=Path, required=True, help='output .tar (must not exist)')
    ap.add_argument('--runtime-commit', required=True, help='AppImage/type2-runtime commit of the runtime used')
    ap.add_argument('--sevenzip-version', default='2603', help='7-Zip version as in 7z<version>-src.tar.xz')
    ap.add_argument('--reuse', type=Path, help='directory of previously downloaded source files to link from')
    args = ap.parse_args()
    if args.out.exists():
        raise SystemExit('Output exists; existing files are preserved.')
    work = args.out.with_suffix('.d')
    if work.exists():
        raise SystemExit('Work directory exists: ' + str(work))
    ubuntu = work / 'ubuntu'
    ubuntu.mkdir(parents=True)
    # apt-get source skips files already present with the right checksum.
    if args.reuse:
        for old in args.reuse.iterdir():
            if old.is_file():
                os.link(old, ubuntu / old.name)

    files = [l for l in (args.stage / 'app/system-files.txt').read_text().splitlines() if l]
    table = owners()
    by_package, unowned = collections.defaultdict(list), []
    for path in files:
        package = table.get(path) or table.get(os.path.realpath(path))
        (by_package[package].append(path) if package else unowned.append(path))
    fields = subprocess.check_output(['dpkg-query', '-W', '-f=${Package}\t${source:Package}\t${source:Version}\t${Version}\n'],
                                     text=True)
    source_of = {}
    for line in fields.splitlines():
        package, src, src_version, version = line.split('\t')
        source_of[package] = (src or package, src_version or version)
    sources = collections.defaultdict(set)
    for package in by_package:
        sources[source_of[package]].add(package)

    # deb-src entries mirroring the image's binary sources.
    lines = [l.replace('deb ', 'deb-src ', 1) for l in Path('/etc/apt/sources.list').read_text().splitlines()
             if l.startswith('deb ')]
    Path('/etc/apt/sources.list.d/battride-sources.list').write_text('\n'.join(lines) + '\n')
    subprocess.run(['apt-get', 'update', '-qq'], check=True)
    failed = []
    for (src, version) in sorted(sources):
        result = subprocess.run(['apt-get', 'source', '--download-only', '-qq', f'{src}={version}'], cwd=ubuntu)
        if result.returncode and not reused_complete(ubuntu, src, version):
            failed.append(f'{src}={version}')
    if failed:
        raise SystemExit('Source packages not available: ' + ', '.join(failed))
    # Keep only the .dsc files and the tarballs they list (drops reused leftovers).
    wanted = set()
    for dsc in ubuntu.glob('*.dsc'):
        if dsc.name.split('_')[0] not in {src for src, _ in sources}:
            continue
        text = dsc.read_text(errors='replace')
        version = next(l.split(':', 1)[1].strip() for l in text.splitlines() if l.startswith('Version:'))
        if (dsc.name.split('_')[0], version) not in sources:
            continue
        wanted.add(dsc.name)
        block = text.split('\nFiles:\n', 1)[1].split('\n\n')[0].split('\n-----', 1)[0]
        wanted.update(l.split()[-1] for l in block.splitlines() if l.startswith(' '))
    for f in ubuntu.iterdir():
        if f.name not in wanted:
            f.unlink()

    extra = work / 'upstream'
    extra.mkdir()
    fetch(f'https://github.com/AppImage/type2-runtime/archive/{args.runtime_commit}.tar.gz',
          extra / f'type2-runtime-{args.runtime_commit}.tar.gz')
    for name, url, sha256 in RUNTIME_DEPS:
        fetch(url, extra / name, sha256)
    fetch(f'https://www.7-zip.org/a/7z{args.sevenzip_version}-src.tar.xz', extra / f'7z{args.sevenzip_version}-src.tar.xz')

    package = json.loads((args.stage / 'app/package.json').read_text())
    out = ['# Corresponding source for Battride War Recomp %s' % package['version'], '',
           'Source code for every third-party binary in `BattrideWarRecomp-v%s-x86_64.AppImage`. '
           'The port\'s own code and the ps3recomp SDK are inside the AppImage in source form '
           '(`usr/share/battride/`) and in the project repository. RPCS3 is not redistributed: '
           'the launcher downloads the official build (source: %s).' % (package['version'], package['rpcs3']['source']),
           '', '## Ubuntu 22.04 source packages (`ubuntu/`)', '',
           'Each entry is a Debian source package (`.dsc` with its tarballs) of exactly the version installed in the '
           'build image. Unpack with `dpkg-source -x <file>.dsc`.', '',
           '| Source package | Version | Binary packages used |', '|---|---|---|']
    for (src, version) in sorted(sources):
        out.append('| %s | %s | %s |' % (src, version, ', '.join(sorted(sources[(src, version)]))))
    out += ['', '## Upstream sources (`upstream/`)', '',
            '- `type2-runtime-%s.tar.gz`: the AppImage runtime (MIT), https://github.com/AppImage/type2-runtime. '
            'Its build compiles in the two libraries below; see `scripts/common/install-dependencies.sh` and '
            '`patches/` in that tarball.' % args.runtime_commit,
            '- `fuse-3.15.0.tar.xz`: libfuse (LGPL-2.1 for the library), linked statically into the runtime.',
            '- `squashfuse-0.5.2.tar.gz`: squashfuse (BSD-2-Clause), linked statically into the runtime.',
            '- `7z%s-src.tar.xz`: 7-Zip (LGPL-2.1 with the unRAR restriction), source of `usr/bin/7z`.' % args.sevenzip_version,
            '', '## Files not owned by a package', '',
            'Generated in the build image, not shipped by a package (for example the compiled GSettings schema):', '']
    out += ['- `%s`' % p for p in unowned] or ['- none']
    (work / 'SOURCES.md').write_text('\n'.join(out) + '\n')

    with tarfile.open(args.out, 'x') as tar:
        tar.add(work, arcname=args.out.stem)
    print('Sources:', args.out, '(%d source packages)' % len(sources))


if __name__ == '__main__':
    main()
