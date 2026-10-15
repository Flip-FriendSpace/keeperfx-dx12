# SPDX-License-Identifier: GPL-2.0-or-later
"""Export accepted replacement pixels and recipes, never original fallback pixels."""
from pathlib import Path
import argparse
import hashlib
import io
import json
import struct
import zipfile

from PIL import Image

REPO = Path(__file__).resolve().parents[2]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def encode_json(value):
    return (json.dumps(value, indent=2) + '\n').encode('utf-8')


def write_checked(path, data):
    if path.exists():
        if path.is_symlink() or path.read_bytes() != data:
            raise ValueError(f'Refusing to replace a different existing export: {path}')
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def export(prototype_root, data_dir, destination):
    combined = prototype_root / 'heroes/mod/heroes_art_prototype'
    source_report = read_json(prototype_root / 'heroes/combined-package-report.json')
    table = (data_dir / 'creature.tab').read_bytes()
    if len(table) % 16:
        raise ValueError('Expected an unpacked 16-byte KeeperFX creature frame table')
    counts = dict(creatures=0, animations=0, four_x_overhead=0, four_x_held=0,
                  enhanced_native_comparison=0, enhanced_files=0, fallback_frames=0)
    records = []
    for package in source_report['packages']:
        slug = package['slug']
        archive_path = combined / 'fxdata' / package['zip']
        if sha(archive_path.read_bytes()) != package['zip_sha256']:
            raise ValueError(f'Accepted package hash differs: {slug}')
        config_bytes = (combined / 'creatrs' / package['config']).read_bytes()
        if sha(config_bytes) != package['config_sha256']:
            raise ValueError(f'Accepted configuration hash differs: {slug}')
        if slug == 'imp':
            source = read_json(prototype_root / 'imp-set/source-manifest.json')
            source.update(read_json(prototype_root / 'imp-actions/source-manifest.json'))
        else:
            source = read_json(prototype_root / 'heroes' / slug / 'source-manifest.json')
        recipe = {'schema': 1, 'slug': slug, 'config': config_bytes.decode('utf-8'),
                  'sprites': [], 'frames': {}, 'canonical_package_sha256': package['zip_sha256']}
        with zipfile.ZipFile(archive_path) as archive:
            if archive.testzip():
                raise ValueError(f'Invalid source ZIP: {slug}')
            recipe['sprites'] = json.loads(archive.read('sprites.json'))
            for animation in recipe['sprites']:
                action = animation['name'].split('_HD_')[-1].split('_NATIVE_')[-1].lower()
                for view in ('td', 'fp'):
                    factor = animation.get(view + '_resolution_scale', animation.get('resolution_scale', 1))
                    enhanced = view == 'td' or factor == 4
                    for direction, sequence in enumerate(animation[view]):
                        for number, frame in enumerate(sequence):
                            member = frame['file']
                            if member in recipe['frames']:
                                raise ValueError(f'Duplicate sprite member: {slug}/{member}')
                            raw = archive.read(member)
                            with Image.open(io.BytesIO(raw)) as opened:
                                image = opened.convert('RGBA')
                            if image.size != (frame['frame_w'], frame['frame_h']):
                                raise ValueError(f'Frame extent differs: {slug}/{member}')
                            if enhanced:
                                relative = f'frames/{slug}/{member}'
                                recipe['frames'][member] = {'enhanced': relative, 'sha256': sha(raw)}
                                write_checked(destination / relative, raw)
                                counts['enhanced_files'] += 1
                                key = ('four_x_overhead' if view == 'td' else 'four_x_held') if factor == 4 else 'enhanced_native_comparison'
                                counts[key] += 1
                            else:
                                if view != 'fp' or factor != 1:
                                    raise ValueError(f'Unexpected original fallback: {slug}/{member}')
                                original = source[action][view][direction][number]
                                index = original['table_index']
                                header = list(struct.unpack_from('<I8Bhh', table, index * 16))
                                if image.size != tuple(original['frame_size']):
                                    raise ValueError(f'Original fallback metadata differs: {slug}/{member}')
                                recipe['frames'][member] = {'original': {
                                    'table_index': index, 'header': header,
                                    'rgba_sha256': sha(image.tobytes()),
                                    'animation': original['animation'], 'view': view,
                                    'direction': direction, 'frame': number}}
                                counts['fallback_frames'] += 1
            referenced = set(recipe['frames']) | {'sprites.json'}
            if referenced != set(archive.namelist()):
                raise ValueError(f'Unaccounted source ZIP members: {slug}')
        recipe_name = f'recipes/{slug}.json'
        encoded = encode_json(recipe)
        write_checked(destination / recipe_name, encoded)
        records.append({'slug': slug, 'recipe': recipe_name, 'sha256': sha(encoded)})
        counts['creatures'] += 1
        counts['animations'] += len(recipe['sprites'])
    expected = dict(creatures=14, animations=158, four_x_overhead=1688,
                    four_x_held=84, enhanced_native_comparison=65, enhanced_files=1837)
    for key, value in expected.items():
        if counts[key] != value:
            raise ValueError(f'Unexpected exported {key}: {counts[key]} != {value}')
    manifest = {'schema': 1, 'id': 'original-heroes-and-imp-4x-v1',
                'canonical_package_manifest_sha256': source_report['package_manifest_sha256'],
                'counts': counts, 'creatures': records,
                'original_data': 'Not included. Fallback frames are decoded from the player\'s compatible local creature.tab, creature.jty and palette.dat.',
                'notice': 'NOTICE.md'}
    write_checked(destination / 'manifest.json', encode_json(manifest))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prototype-root', type=Path, default=REPO.parent / 'artifacts/art-prototype')
    parser.add_argument('--data', type=Path, default=REPO.parent / 'runtime/keeperfx-x64/data')
    parser.add_argument('--destination', type=Path, default=REPO / 'assets/creatures')
    args = parser.parse_args()
    result = export(args.prototype_root.resolve(), args.data.resolve(), args.destination.resolve())
    print(json.dumps(result['counts'], indent=2))


if __name__ == '__main__':
    main()
