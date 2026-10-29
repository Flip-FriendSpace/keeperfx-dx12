# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only independent CTA package checks and local review-page creation."""
import argparse
import hashlib
import html
import io
import json
from pathlib import Path
import tomllib
import zipfile

import numpy as np
from PIL import Image


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify(manifest_path, register_path, red_manifest_path):
    manifest = json.loads(manifest_path.read_text())
    register = json.loads(register_path.read_text())
    source_path = Path(manifest['source_manifest'])
    assert digest(source_path) == manifest['source_manifest_sha256']
    assert digest(register_path) == manifest['source_register_sha256']
    source = json.loads(source_path.read_text())
    red = json.loads(red_manifest_path.read_text())
    archive = Path(manifest['mod_root']) / 'fxdata/cta-owners-sprites.zip'
    config = archive.with_name('spritecolors.toml')
    assert digest(archive) == manifest['zip_sha256']
    assert digest(config) == manifest['config_sha256']
    colours = ['red', 'blue', 'green', 'yellow', 'white', 'purple', 'black', 'orange']
    phases = ['birth', 'alive', 'leave']
    assert [o['colour'] for o in register['owners']] == colours[1:]
    rows = tomllib.loads(config.read_text())['animationIds']
    assert len(rows) == 3
    for i, phase in enumerate(phases):
        names = ['FIDELITY_CTA_'+c.upper()+'_'+phase.upper() for c in colours]
        names.insert(5, 0)
        assert rows[i] == [867+i, *names]
    receipts = []
    with zipfile.ZipFile(archive) as z, zipfile.ZipFile(Path(red['mod_root'])/'fxdata/cta-red-sprites.zip') as rz:
        assert z.testzip() is None
        entries = json.loads(z.read('sprites.json'))
        assert len(entries) == 24
        assert len(z.namelist()) == len(set(z.namelist())) == 385
        assert entries[:3] == json.loads(rz.read('sprites.json'))
        for name in rz.namelist():
            if name != 'sprites.json':
                assert z.read(name) == rz.read(name)
        by_name = {e['name']: e for e in entries}
        for asset in source['assets']:
            key = asset['key']
            entry = by_name['FIDELITY_CTA_'+key.upper().replace('-', '_')]
            assert entry['resolution_scale'] == 4 and entry['rotatable'] is False
            for sequence in asset['sequences']:
                view = sequence['view']
                frames = entry[view][0]
                assert len(entry[view]) == 1
                assert len(frames) == len(sequence['frames']) == {'birth': 7, 'alive': 8, 'leave': 9}[asset['phase']]
                native, candidate = [], []
                for i, (original, frame) in enumerate(zip(sequence['frames'], frames)):
                    source_file = source_path.parent/key/original['file']
                    assert digest(source_file) == original['png_sha256']
                    src = Image.open(source_file).convert('RGBA')
                    ref = np.asarray(src.resize((src.width*4, src.height*4), Image.Resampling.NEAREST))
                    im = Image.open(io.BytesIO(z.read(frame['file']))).convert('RGBA')
                    pixels = np.asarray(im)
                    assert frame['file'] == f'{key}/{view}-d0-f{i:02}.png'
                    assert list(im.size) == [v*4 for v in original['frame_size']]
                    assert im.size == (frame['frame_w'], frame['frame_h'])
                    assert [frame['offset_x'], frame['offset_y']] == [-v*4 for v in original['anchor']]
                    assert frame['offset_w'] == frame['offset_h'] == 0
                    assert frame['shadow_offset'] == original.get('shadow_offset', 0)*4
                    assert frame.get('frame_flags', 0) == original.get('frame_flags', 0)
                    assert set(np.unique(pixels[:, :, 3])).issubset({0, 255})
                    assert np.array_equal(pixels[:, :, 3] >= 64, ref[:, :, 3] >= 64)
                    native.append(ref)
                    candidate.append(pixels)
                    receipts.append({'key': key, 'view': view, 'frame': i, 'source_sha256': digest(source_file),
                                     'png_sha256': hashlib.sha256(z.read(frame['file'])).hexdigest(), 'anchor': original['anchor']})
                native, candidate = np.stack(native), np.stack(candidate)
                stable = np.all(native == native[0], axis=(0, 3))
                assert np.all(candidate[:, stable] == candidate[0, stable])
    assert len(receipts) == 384
    return {'schema': 1, 'passed': True, 'zip_sha256': digest(archive), 'config_sha256': digest(config),
            'frames_checked': 384, 'owners_checked': colours, 'red_members_and_metadata_unchanged': True,
            'all_native_masks_dimensions_anchors_phase_order_exact': True, 'whole_sequence_stationary_changes': 0,
            'neutral_mapping_zero': True, 'motion_colour_delta_equivalence_claimed': False,
            'runtime_validated': False, 'user_appearance_accepted': False, 'frames': receipts}


def review(manifest_path, register_path):
    target = manifest_path.parent
    manifest = json.loads(manifest_path.read_text())
    register = json.loads(register_path.read_text())
    cards = []
    for owner in register['owners']:
        colour = owner['colour']
        cards.append('<h2>'+html.escape(colour.title())+'</h2>')
        for sequence in owner['sequences']:
            stem = sequence['key']+'-'+sequence['view']
            cards.append('<article><h3>'+html.escape(stem)+'</h3><p>Original / candidate at native size. 50 ms preview per frame.</p>'
                         '<img class="loop" src="'+stem+'-loop.gif"><details><summary>All frames at 4x</summary>'
                         '<img class="board" src="'+stem+'-comparison.png"></details></article>')
    document = '<!doctype html><meta charset="utf-8"><title>Call to Arms owner review</title><style>body{background:#202328;color:#ddd;font:16px system-ui;max-width:1200px;margin:auto;padding:24px}h1,h2{color:#fff}article{display:inline-block;vertical-align:top;width:30%;margin:1%;padding-bottom:18px}.loop{image-rendering:pixelated;min-height:104px}.board{max-width:100%}summary{cursor:pointer}code{overflow-wrap:anywhere}</style>'
    document += '<h1>Call to Arms — all owner colours</h1><p>Local candidate only. 384 frame uses across eight owners; red remains byte-identical to the previous reviewed candidate. Neutral retains its original zero mapping. The seven other owners each use their own original frame masks and generated sheets.</p><p>Original 7/8/9 frame lifecycle, dimensions, anchors and stationary regions are preserved. These loops illustrate frame order; the engine owns simulation timing, costs and attraction. No gameplay configuration changed.</p><p>Archive SHA256: <code>'+manifest['zip_sha256']+'</code></p><p>Technical checks do not constitute user appearance acceptance. Live lifecycle validation is recorded separately.</p>'
    document += ''.join(cards)
    with (target/'review.html').open('x', encoding='utf-8') as stream:
        stream.write(document)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--register', type=Path, required=True)
    ap.add_argument('--red-manifest', type=Path, required=True)
    ap.add_argument('--report', type=Path, required=True)
    ap.add_argument('--review', action='store_true')
    args = ap.parse_args()
    if not __debug__:
        ap.error('Run without -O; preservation assertions must remain enabled')
    report = verify(args.manifest, args.register, args.red_manifest)
    with args.report.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    if args.review:
        review(args.manifest, args.register)
    print(json.dumps({k: v for k, v in report.items() if k != 'frames'}))


if __name__ == '__main__':
    main()
