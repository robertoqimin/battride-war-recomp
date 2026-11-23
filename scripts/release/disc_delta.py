#!/usr/bin/env python3
"""Write packaging/discs/<title>.json: what another edition's disc changes from
the base edition's (BLJS10220), as names, sizes and SHA-256 only. The packages
ship it beside the base disc manifest; the importer rebuilds that edition's
full list from both (importer.apply_delta).

usage: disc_delta.py <base game folder> <other game folder> <title id>

Each folder holds an extracted disc (PS3_GAME/, PS3_DISC.SFB).
"""
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'packaging'))
import importer  # noqa: E402

if len(sys.argv) != 4:
    sys.exit(__doc__)
base = importer.disc_manifest(sys.argv[1])
other = importer.disc_manifest(sys.argv[2])
delta = importer.manifest_delta(base, other, sys.argv[3], 'BLJS10220')
out = Path(__file__).resolve().parents[2] / 'packaging/discs' / (sys.argv[3] + '.json')
out.write_text(json.dumps(delta, indent=1, sort_keys=True) + '\n')
print('%s: %d changed, %d added, %d removed (%s)' % (out.name, len(delta['changed']), len(delta['added']),
                                                     len(delta['removed']), out))
