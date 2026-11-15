#!/usr/bin/env python3
"""Audit an explicit public-file list and optionally create a source-only ZIP."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PRIVATE = {'game', 'firmware', 'gamedata', 'analysis', 'build', 'build-vulkan',
           'spu', 'src', '.git', '.venv'}
EXTENSIONS = {'.c', '.cpp', '.h', '.hpp', '.md', '.txt', '.toml', '.json', '.sh', '.py', '.patch'}
SPECIAL = {'.gitignore', 'LICENSE'}

def public_files():
    files = []
    seen = set()
    for line in (ROOT / 'PUBLIC_FILES.txt').read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        name = line.strip()
        relative = PurePosixPath(name)
        if (relative.is_absolute() or '..' in relative.parts or '\\' in name or
                relative.parts[0] in PRIVATE or name in seen or
                (relative.suffix not in EXTENSIONS and name not in SPECIAL)):
            raise ValueError(f'not an allowed public path: {name}')
        path = ROOT.joinpath(*relative.parts)
        if any(parent.is_symlink() for parent in (path, *path.parents)):
            raise ValueError(f'symlink not allowed: {name}')
        if not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
            raise ValueError(f'not a regular source file under 2 MiB: {name}')
        content = path.read_bytes()
        content.decode('utf-8')  # Reject binary payloads, even with a source extension.
        if b'\x00' in content:
            raise ValueError(f'binary content: {name}')
        files.append((name, content))
        seen.add(name)
    if not files:
        raise ValueError('empty public file list')
    return files

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, help='new ZIP filename; never overwritten')
    ap.add_argument('--manifest', type=Path, help='new JSON manifest filename; never overwritten')
    args = ap.parse_args()
    files = public_files()
    for path in (args.output, args.manifest):
        if path is not None and path.exists():
            ap.error(f'refusing to overwrite {path}')
    manifest = {'files': [{'path': n, 'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
                          for n, b in files], 'total_bytes': sum(len(b) for _, b in files)}
    if args.output:
        with zipfile.ZipFile(args.output, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, content in files:
                info = zipfile.ZipInfo('kamenriderbattridewarsport/' + name)
                info.create_system = 3
                mode = 0o100755 if Path(name).suffix in {'.sh', '.py'} else 0o100644
                info.external_attr = mode << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, content)
    if args.manifest:
        with args.manifest.open('x') as stream:
            json.dump(manifest, stream, indent=2)
            stream.write('\n')
    print(f'OK: {len(files)} explicit source/documentation files, {manifest["total_bytes"]} bytes')
    if args.output:
        print(f'ZIP: {args.output}')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, UnicodeError) as exc:
        print(f'publication audit failed: {exc}', file=sys.stderr)
        sys.exit(1)
