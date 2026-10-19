# SPDX-License-Identifier: GPL-2.0-or-later
"""Store a match_sprite_colour.py pass in the remaster sprite and UI bundles.

The graded files must come from outputs built from the current bundles. Each changed
PNG member becomes a lossless pixel object in its archive recipe, after checking that
only enhanced members changed and that alpha is untouched. Each graded campaign frame
gets a new generated-index layer that reproduces it exactly. Everything else stays byte
for byte. A later sprite pass may only grade archives that no earlier pass changed; it is
appended to the bundle's colour_match record. Rebuild both bundles afterwards and compare
the outputs with the graded files.
"""
from pathlib import Path
import argparse
import io
import json
import sys
import zipfile
import zlib

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
sys.path.insert(0, str(REPO / 'local-dev/world-remaster/frontend-art'))
from remaster_sprite_codec import (AssetError, digest, encode_json, parse_json, read_regular,  # noqa: E402
                                   load_object, store_object, object_path, rgba_image, png_bytes,
                                   pixel_streams, decode_pixel_stream, reconstruct_member)
import ui_publication  # noqa: E402

ENCODER = {'compress_level': 6, 'optimize': False}


def recipes(assets, manifest):
    return [parse_json(load_object(assets, record['recipe']), 'archive recipe') for record in manifest['archives']]


def sprite_objects(manifest, all_recipes):
    refs = {record['recipe']['sha256'] for record in manifest['archives']}
    refs.update(record['object']['sha256'] for record in manifest['files'])
    for recipe in all_recipes:
        for frame in recipe['members']:
            refs.update(frame['source'][key]['sha256'] for key in ('object', 'pixels') if key in frame['source'])
    return refs


def graded_member(assets, frame, png, published):
    """Lossless source for a graded PNG whose alpha and size match the bundle's member."""
    if frame['source']['kind'] != 'enhanced':
        raise AssetError('Colour match changed a non-enhanced member: ' + frame['name'])
    before = rgba_image(reconstruct_member(assets, frame, None, published))
    image = rgba_image(png)
    if image.size != before.size or image.getchannel('A').tobytes() != before.getchannel('A').tobytes():
        raise AssetError('Colour match changed size or alpha: ' + frame['name'])
    if png_bytes(image, ENCODER) != png:
        raise AssetError('Graded PNG is not reproducible by the pinned encoder: ' + frame['name'])
    pixels = next(pixel_streams(image))
    restored = decode_pixel_stream(pixels)
    if restored.tobytes() != image.tobytes() or png_bytes(restored, ENCODER) != png:
        raise AssetError('Lossless pixel reconstruction differs: ' + frame['name'])
    if len(zlib.compress(pixels, 9)) < len(zlib.compress(png, 9)):
        source = {'kind': 'enhanced', 'pixels': store_object(assets, pixels), 'encoder': dict(ENCODER)}
    else:
        source = {'kind': 'enhanced', 'object': store_object(assets, png)}
    return source, image


def apply_sprites(assets, graded, report, report_sha):
    raw_manifest = read_regular(assets / 'manifest.json')
    manifest = parse_json(raw_manifest, 'manifest.json')
    earlier = manifest.get('colour_match')
    matched = set()
    for previous in [earlier, *earlier.get('later_passes', [])] if earlier else []:
        matched.update(previous['archives'])
    published = manifest.get('published_pngs', {})
    before_recipes = recipes(assets, manifest)
    before_objects = sprite_objects(manifest, before_recipes)
    files = {record['file'] for record in manifest['archives']}
    archives = {'fxdata/' + p.name for p in (graded / 'fxdata').glob('*.zip')}
    if archives - files:
        raise AssetError('Graded archives missing from the bundle: ' + ', '.join(sorted(archives - files)))
    if archives & matched:
        raise AssetError('Archives already colour-matched: ' + ', '.join(sorted(archives & matched)))
    changes, after_recipes = {}, []
    for record, recipe in zip(manifest['archives'], before_recipes):
        path = graded / record['file']
        if not path.exists():
            after_recipes.append(recipe)
            continue
        raw = read_regular(path)
        name = Path(record['file']).name
        changed = 0
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            if archive.namelist() != [frame['name'] for frame in recipe['members']]:
                raise AssetError('Graded archive members or order differ: ' + record['file'])
            for frame in recipe['members']:
                png = archive.read(frame['name'])
                if digest(png) == frame['sha256']:
                    continue
                source, image = graded_member(assets, frame, png, published)
                frame.update(sha256=digest(png), bytes=len(png), rgba_sha256=digest(image.tobytes()), source=source)
                changed += 1
        grade = report['archives'][name].get('family')
        changes[record['file']] = {'before_zip_sha256': record['reviewed_zip_sha256'], 'after_zip_sha256': digest(raw),
                                   'changed_members': changed,
                                   'grade': grade and {'gains': grade['gains'], 'chroma_scale': grade['chroma_scale']},
                                   'unit_grades': [{'sprites': unit['sprites'], 'gains': unit['applied']['gains'],
                                                    'chroma_scale': unit['applied']['chroma_scale']}
                                                   for unit in report['archives'][name]['units'] if unit.get('applied')]}
        recipe['reviewed_zip_sha256'] = digest(raw)
        record.update(recipe=store_object(assets, encode_json(recipe)), reviewed_zip_sha256=digest(raw))
        after_recipes.append(recipe)
        print(f"{record['file']}: {changed} members", flush=True)
    used = {frame['source']['published']['file'] for recipe in after_recipes for frame in recipe['members']
            if 'published' in frame['source']}
    manifest['published_pngs'] = {name: value for name, value in published.items() if name in used}
    colour_pass = {
        'tool': 'local-dev/remaster/match_sprite_colour.py', 'tool_sha256': report['tool_sha256'],
        'report_sha256': report_sha, 'previous_manifest_sha256': digest(raw_manifest), 'archives': changes,
        'note': 'Colour-matched archives record the graded archive as reviewed_zip_sha256; member bytes are authoritative.'}
    if earlier:
        earlier.setdefault('later_passes', []).append(colour_pass)
    else:
        manifest['colour_match'] = colour_pass
    after_objects = sprite_objects(manifest, after_recipes)
    (assets / 'manifest.json').write_bytes(encode_json(manifest))
    for sha in sorted(before_objects - after_objects):
        object_path(assets, sha).unlink()
    stored = {p.stem for p in (assets / 'objects').rglob('*.z')}
    if stored != after_objects:
        raise AssetError('Sprite object store differs from the manifest references')
    provenance_path = assets / 'provenance.json'
    provenance = parse_json(read_regular(provenance_path), 'provenance.json')
    summary = {
        'previous_publication_manifest_sha256': digest(raw_manifest), 'report_sha256': report_sha,
        'archives': len(changes), 'members': sum(c['changed_members'] for c in changes.values()),
        'published_pngs_still_reused': len(manifest['published_pngs']),
        'scope': 'Enhanced PNG colour only; original fallback members, alpha, sprites.json, configuration and member order unchanged.'}
    if earlier:
        provenance['colour_match'].setdefault('later_passes', []).append(summary)
    else:
        provenance['colour_match'] = summary
    provenance['publication_manifest_sha256'] = digest(encode_json(manifest))
    provenance_path.write_bytes(encode_json(provenance).replace(b'\n', b'\r\n'))
    return {'archives': len(changes), 'members': sum(c['changed_members'] for c in changes.values()),
            'objects_added': len(after_objects - before_objects), 'objects_removed': len(before_objects - after_objects),
            'published_pngs': len(manifest['published_pngs']), 'manifest_sha256': digest(encode_json(manifest))}


def apply_frames(assets, graded, before_ui, report, report_sha):
    raw_manifest = read_regular(assets / 'manifest.json')
    manifest = json.loads(raw_manifest)
    if 'colour_match' in manifest:
        raise AssetError('The UI bundle is already colour-matched')
    objects = manifest['objects']
    load = lambda key: ui_publication.load_object(assets, objects, key)
    replaced, frames = {}, []
    for entry in manifest['native']:
        path = graded / entry['output']
        if not path.exists():
            continue
        if entry['format'] != 'huge':
            raise AssetError('Only huge-format frames can be colour-matched: ' + entry['output'])
        width, height = entry['dimensions']
        before = read_regular(before_ui / entry['output'])
        if digest(before) != entry['sha256']:
            raise AssetError('Pre-match UI output differs from the manifest: ' + entry['output'])
        after = read_regular(path)
        old_indices, old_mask = ui_publication.decode_huge(before, width, height)
        new_indices, new_mask = ui_publication.decode_huge(after, width, height)
        if new_mask != old_mask:
            raise AssetError('Colour match changed frame coverage: ' + entry['output'])
        shape = (height, width)
        old = np.frombuffer(bytes(old_indices), np.uint8).reshape(shape)
        new = np.frombuffer(bytes(new_indices), np.uint8).reshape(shape)
        changed = (old != new) & (np.frombuffer(bytes(new_mask), np.uint8).reshape(shape) > 0)
        layer = np.frombuffer(load(entry['generated_indices']), np.uint8).reshape(shape).copy()
        if (layer[changed] != old[changed]).any():
            raise AssetError('Colour match changed pixels outside the generated layer: ' + entry['output'])
        layer[changed] = new[changed]
        data = layer.tobytes()
        key = digest(data)
        packed = zlib.compress(data, 9)
        target = assets / 'objects' / (key + '.zlib')
        if target.exists() and target.read_bytes() != packed:
            raise AssetError('Existing UI object differs: ' + key)
        target.write_bytes(packed)
        replaced[entry['generated_indices']] = (key, {'bytes': len(data), 'compressed_bytes': len(packed),
                                                      'compressed_sha256': digest(packed),
                                                      'role': objects[entry['generated_indices']]['role']})
        entry.update(generated_indices=key, bytes=len(after), sha256=digest(after),
                     pixel_sha256=ui_publication.pixel_digest(new_indices, new_mask))
        frames.append(entry['output'])
    # Keep the object table's order; old layers go only when nothing else names them.
    text = json.dumps({k: v for k, v in manifest.items() if k != 'objects'})
    text += ''.join(load(k).decode('latin-1') for k, v in objects.items() if 'metadata' in v['role'])
    table = {}
    for key, value in objects.items():
        if key in replaced:
            new_key, new_value = replaced[key]
            table[new_key] = new_value
            if key in text:
                table[key] = value
        else:
            table[key] = value
    removed = [key for key in replaced if key not in table]
    manifest['objects'] = table
    family = report['frames']['family']
    manifest['colour_match'] = {
        'tool': 'local-dev/remaster/match_sprite_colour.py', 'tool_sha256': report['tool_sha256'],
        'report_sha256': report_sha, 'previous_manifest_sha256': digest(raw_manifest), 'frames': frames,
        'grade': {'gains': family['gains'], 'chroma_scale': family['chroma_scale']},
        'note': 'Generated pixels only, re-quantized to palette entries each frame already used; native pixels and coverage unchanged.'}
    raw = (json.dumps(manifest, indent=2) + '\n').replace('\n', '\r\n').encode()
    (assets / 'manifest.json').write_bytes(raw)
    for key in removed:
        (assets / 'objects' / (key + '.zlib')).unlink()
    return {'frames': len(frames), 'objects_removed': len(removed), 'manifest_sha256': digest(raw)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graded', type=Path, required=True, help='match_sprite_colour.py output')
    parser.add_argument('--before-ui', type=Path, help='ui_publication.py output that was graded')
    parser.add_argument('--sprite-assets', type=Path, default=REPO / 'assets/remaster-sprites')
    parser.add_argument('--ui-assets', type=Path, default=REPO / 'assets/remaster-ui')
    args = parser.parse_args()
    report_raw = read_regular(args.graded / 'colour-match-report.json')
    report, report_sha = json.loads(report_raw), digest(report_raw)
    result = {'sprites': apply_sprites(args.sprite_assets, args.graded, report, report_sha)}
    if report.get('frames'):
        if args.before_ui is None:
            parser.error('--before-ui is required for graded frames')
        result['ui'] = apply_frames(args.ui_assets, args.graded, args.before_ui, report, report_sha)
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
