#!/usr/bin/env python3
"""Verify the vendored MIT sources against the reviewed commit's file hashes."""
import hashlib
from pathlib import Path
root = Path(__file__).resolve().parents[1]
for line in (root / 'md4c.lock').read_text().splitlines()[2:]:
    checksum, name = line.split('  ', 1)
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != checksum:
        raise SystemExit('md4c pin mismatch: ' + name)
print('md4c: pinned source and license hashes match')
