# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Retain reviewed window-capture packets in an existing UI reference run.

Packets contain id, cases, steps, notes, captureSize and a PNG/JPEG data URL.
Keep packets and images in ignored artifacts: they contain game artwork.
Use --accept-reviewed only after inspecting every supplied image.
"""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path

from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('packets', type=Path)
parser.add_argument('--accept-reviewed', action='store_true')
args = parser.parse_args()
directory = args.packets.resolve().parent
run_path = directory / 'run.json'
run_bytes = run_path.read_bytes()
run = json.loads(run_bytes.decode('utf-8-sig'))
run_hash = hashlib.sha256(run_bytes).hexdigest()
manifest_path = directory / 'ui-reference.json'
manifest = json.loads(manifest_path.read_text(encoding='utf-8')) if manifest_path.exists() else dict(
    version=1, renderer=run['renderer'], language=run['language'], runSha256=run_hash, frames=[])
assert manifest['runSha256'] == run_hash
assert (manifest['renderer'], manifest['language']) == (run['renderer'], run['language'])
packets = json.loads(args.packets.read_text(encoding='utf-8'))
if isinstance(packets, dict):
    packets = [packets]
retained = {frame['id'] for frame in manifest['frames']}
prepared = []
for packet in packets:
    frame = dict(packet)
    identifier = frame['id']
    assert identifier and all(c in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in identifier)
    assert identifier not in retained, f'Duplicate reference: {identifier}'
    assert frame['cases'] and frame['steps'] and frame['notes']
    mime, encoded = frame.pop('url').split(',', 1)
    assert mime in ('data:image/jpeg;base64', 'data:image/png;base64')
    data = base64.b64decode(encoded, validate=True)
    extension = '.jpg' if 'jpeg' in mime else '.png'
    image_path = directory / (identifier + extension)
    assert not image_path.exists(), f'Do not overwrite retained evidence: {image_path}'
    with Image.open(io.BytesIO(data)) as image:
        image.load()
        assert list(image.size) == frame['captureSize']
        assert image.format == ('JPEG' if extension == '.jpg' else 'PNG')
    frame.update(image=image_path.name, imageSha256=hashlib.sha256(data).hexdigest(),
                 decision='accepted' if args.accept_reviewed else 'pending',
                 captureMethod='Windows window capture, including frame and logical DPI scaling. Composition reference only; not an exact-pixel oracle.')
    prepared.append((image_path, data, frame))
    retained.add(identifier)
for image_path, data, frame in prepared:
    image_path.write_bytes(data)
    manifest['frames'].append(frame)
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(f'Retained {len(prepared)} UI references in {directory}')
