# SPDX-License-Identifier: GPL-2.0-or-later
"""Replace or explicitly add reviewed sprite archives and configuration files.

Each updated archive is read whole. Members whose bytes are unchanged keep their recorded
source. Changed or new members are classified as the exporter does: pixels that the
player's original data reproduces become recipes, fully transparent frames become
transparent recipes, and the rest become lossless enhanced objects. Original fallbacks
that cannot be reconstructed are refused, never published: a member the bundle rebuilt
from original data may become enhanced only when it is named with --new-art. The update is recorded in the
manifest under archive_updates with the previous manifest hash and each file's hashes
before and after. Unreferenced objects are removed. Rebuild the bundle afterwards and
compare the outputs with the source files.
"""
from pathlib import Path
import argparse
import collections
import io
import json
import sys
import zipfile
import zlib

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from remaster_sprite_codec import (AssetError, OriginalBank, digest, encode_json, parse_json,  # noqa: E402
                                   read_regular, safe_relative, store_object, object_path, rgba_image,
                                   png_bytes, pixel_streams, decode_pixel_stream, SCHEMA, PUBLICATION_IDENTITIES)
from export_remaster_sprites import OriginalIndex, encoder_for  # noqa: E402
from apply_colour_match import recipes, sprite_objects  # noqa: E402


def contexts_of(sprites):
    contexts = collections.defaultdict(list)
    for entry in sprites:
        for view in ('td', 'fp'):
            scale = entry.get(view + '_resolution_scale', entry.get('resolution_scale', 1))
            if scale not in (1, 2, 4, 8):
                raise AssetError('Unsupported view resolution scale')
            for direction, sequence in enumerate(entry[view]):
                for phase, frame in enumerate(sequence):
                    contexts[frame['file']].append((view, scale, entry['name'], direction, phase))
    return contexts


def enhanced_source(assets, image, png, reusable):
    """A PNG the repository already publishes, else the smaller lossless store: indexed or
    RGBA pixels when a supported encoder restores the PNG, or the PNG itself."""
    if digest(png) in reusable:
        name = reusable[digest(png)]
        return {'kind': 'enhanced', 'published': {'file': name, 'sha256': digest(png), 'bytes': len(png)}}
    try:
        encoder = encoder_for(image, png)
    except AssetError:
        encoder = None
    if encoder is not None:
        pixels = next(pixel_streams(image))
        restored = decode_pixel_stream(pixels)
        if (restored.tobytes() == image.tobytes() and png_bytes(restored, encoder) == png and
                len(zlib.compress(pixels, 9)) < len(zlib.compress(png, 9))):
            return {'kind': 'enhanced', 'pixels': store_object(assets, pixels), 'encoder': encoder}
    return {'kind': 'enhanced', 'object': store_object(assets, png)}


def member_source(assets, index, relative, name, png, contexts, reusable, was_original, new_art,
                  added_archive=False):
    if name == 'sprites.json':
        return {'kind': 'metadata', 'object': store_object(assets, png)}, None
    image = rgba_image(png)
    native = index.match(image)
    if native is not None:
        return {'kind': 'original', 'recipe': native, 'encoder': encoder_for(image, png)}, image
    if not image.getchannel('A').getbbox() and len(image.getcolors(maxcolors=2) or []) == 1:
        return {'kind': 'transparent', 'canvas': list(image.size), 'rgba': list(image.getpixel((0, 0))),
                'encoder': encoder_for(image, png)}, image
    lowres_fp = any(view == 'fp' and scale == 1 for view, scale, *_ in contexts[name])
    if lowres_fp or name.startswith('references/') or (was_original and (relative, name) not in new_art):
        raise AssetError(f'Original fallback cannot safely reconstruct: {relative}:{name}')
    if added_archive and (not contexts[name] or any(scale == 1 for _, scale, *_ in contexts[name])):
        raise AssetError(f'New archive art must be referenced at enhanced scale: {relative}:{name}')
    return enhanced_source(assets, image, png, reusable), image


def update(assets, source, data, archives, files, note, new_art=(), *, add_archives=(), add_files=(),
           publication_identity=None):
    """Apply the update. A failure before the manifest is written removes the objects stored
    so far, leaving the bundle as it was."""
    raw_manifest = read_regular(assets / 'manifest.json')
    stored = set((assets / 'objects').rglob('*.z'))
    try:
        return apply_update(assets, source, data, archives, files, note, set(new_art), raw_manifest,
                            add_archives, add_files, publication_identity)
    except BaseException:
        if read_regular(assets / 'manifest.json') == raw_manifest:
            for path in set((assets / 'objects').rglob('*.z')) - stored:
                path.unlink()
        raise


def apply_update(assets, source, data, archives, files, note, new_art, raw_manifest,
                  add_archives, add_files, publication_identity):
    manifest = parse_json(raw_manifest, 'manifest.json')
    records = {record['file']: record for record in manifest['archives']}
    configs = {record['file']: record for record in manifest['files']}
    unknown = sorted(set(archives) - set(records)) + sorted(set(files) - set(configs))
    if unknown:
        raise AssetError('Not in the bundle: ' + ', '.join(unknown))
    selected = [*archives, *files, *add_archives, *add_files]
    if not selected or len({name.casefold() for name in selected}) != len(selected):
        raise AssetError('Select distinct archive/configuration paths')
    existing = {name.casefold() for name in [*records, *configs]}
    for name in [*add_archives, *add_files]:
        safe_relative(name)
        if name.casefold() in existing:
            raise AssetError('Addition collides with an existing output: ' + name)
        if len(name.split('/')) != 2:
            raise AssetError('Added outputs must be directly inside fxdata/ or creatrs/')
        if name in add_archives:
            if not name.startswith('fxdata/') or not name.endswith('.zip'):
                raise AssetError('Added sprite archives must be fxdata/*.zip')
        elif not (name.startswith(('fxdata/', 'creatrs/')) and name.endswith(('.cfg', '.toml'))):
            raise AssetError('Added configurations must be fxdata/ or creatrs/ .cfg/.toml files')
    previous_identity = {key: manifest[key] for key in ('id', 'authority_manifest_sha256')}
    if publication_identity is not None:
        if publication_identity not in PUBLICATION_IDENTITIES:
            raise AssetError('Unsupported publication identity')
        manifest.update(id=publication_identity,
                        authority_manifest_sha256=PUBLICATION_IDENTITIES[publication_identity])
    published = dict(manifest.get('published_pngs', {}))
    # Same reuse rule as the compact repack: identical PNGs already in assets/creatures.
    root, reusable = assets.parents[1], {}
    for path in sorted((root / 'assets/creatures').rglob('*.png')):
        reusable.setdefault(digest(read_regular(path)), path.relative_to(root).as_posix())
    before_recipes = recipes(assets, manifest)
    before_objects = sprite_objects(manifest, before_recipes)
    matched = set()
    earlier = manifest.get('colour_match')
    for previous in [earlier, *earlier.get('later_passes', [])] if earlier else []:
        matched.update(previous['archives'])
    if manifest['counts'] != dict(sum((collections.Counter(r['counts']) for r in manifest['archives']),
                                      collections.Counter())):
        raise AssetError('Manifest counts differ from the archive records')
    index = OriginalIndex(OriginalBank(data, manifest['original_files_sha256'],
                                       manifest['original_archives_sha256'])) if archives or add_archives else None
    changes, after_recipes, confirmed = {}, [], set()
    added_records = [{'file': name} for name in add_archives]
    added_recipes = [{'schema': SCHEMA, 'archive': name, 'reviewed_zip_sha256': None, 'members': []}
                     for name in add_archives]
    manifest['archives'].extend(added_records)
    for record, recipe in zip(manifest['archives'], [*before_recipes, *added_recipes]):
        if record['file'] not in [*archives, *add_archives]:
            after_recipes.append(recipe)
            continue
        raw = read_regular(source / safe_relative(record['file']))
        kept = {frame['name']: frame for frame in recipe['members']}
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            names = archive.namelist()
            if archive.testzip() is not None or len({n.casefold() for n in names}) != len(names):
                raise AssetError('Bad or duplicate source ZIP member: ' + record['file'])
            sprites_raw = archive.read('sprites.json')
            contexts = contexts_of(parse_json(sprites_raw, 'sprites.json'))
            extras = set(names) - {'sprites.json', *contexts}
            if not set(contexts) <= set(names) or any(not n.endswith('.png') for n in extras):
                raise AssetError('Unaccounted non-PNG archive members: ' + record['file'])
            members, tally, changed = [], collections.Counter(), collections.Counter()
            for name in names:
                safe_relative(name)
                png = archive.read(name)
                frame = kept.get(name)
                if frame is None or frame['sha256'] != digest(png):
                    was_original = frame is not None and frame['source']['kind'] == 'original'
                    member, image = member_source(assets, index, record['file'], name, png, contexts, reusable,
                                                  was_original, new_art, record['file'] in add_archives)
                    if was_original and member['kind'] == 'enhanced':
                        confirmed.add((record['file'], name))
                    if 'published' in member:
                        published[member['published']['file']] = {'sha256': member['published']['sha256'],
                                                                   'bytes': member['published']['bytes']}
                    frame = {'name': name, 'sha256': digest(png), 'bytes': len(png), 'source': member}
                    if image is not None:
                        frame.update(size=list(image.size), rgba_sha256=digest(image.tobytes()))
                    changed[member['kind']] += 1
                frame = {**frame, 'referenced': name == 'sprites.json' or name in contexts}
                members.append(frame)
                tally[frame['source']['kind']] += 1
        before_zip = recipe['reviewed_zip_sha256']
        recipe = {**recipe, 'reviewed_zip_sha256': digest(raw), 'members': members}
        record.update(recipe=store_object(assets, encode_json(recipe)), reviewed_zip_sha256=digest(raw),
                      members=len(members), counts=dict(tally))
        after_recipes.append(recipe)
        changes[record['file']] = {'before_zip_sha256': before_zip,
                                   'after_zip_sha256': digest(raw), 'changed_members': sum(changed.values()),
                                   'changed_by_source': dict(sorted(changed.items())),
                                   'removed_members': len(set(kept) - set(names))}
        print(f"{record['file']}: {dict(changed)} changed, {dict(tally)} in all", flush=True)
    if confirmed != new_art:
        raise AssetError('--new-art names a member that is not a replaced original fallback: ' +
                         ', '.join(':'.join(m) for m in sorted(new_art - confirmed)))
    config_changes = {}
    for name in [*files, *add_files]:
        if name in add_files:
            record = {'file': name, 'sha256': None}
            manifest['files'].append(record)
        else:
            record = configs[name]
        content = read_regular(source / safe_relative(name))
        config_changes[name] = {'before_sha256': record['sha256'], 'after_sha256': digest(content)}
        record.update(sha256=digest(content), bytes=len(content), object=store_object(assets, content))
    used_archives = {}
    for recipe in after_recipes:
        for frame in recipe['members']:
            native = frame['source'].get('recipe', {})
            if frame['source']['kind'] == 'original' and 'archive' in native:
                used_archives[native['archive']] = native['archive_sha256']
    manifest['original_archives_sha256'] = dict(sorted(used_archives.items()))
    manifest['counts'] = dict(sum((collections.Counter(r['counts']) for r in manifest['archives']),
                                  collections.Counter()))
    used = {frame['source']['published']['file'] for recipe in after_recipes for frame in recipe['members']
            if 'published' in frame['source']}
    if 'published_pngs' in manifest or used:
        manifest['published_pngs'] = {name: value for name, value in sorted(published.items()) if name in used}
    entry = {'tool': 'local-dev/remaster/apply_archive_update.py', 'tool_sha256': digest(read_regular(Path(__file__))),
             'previous_manifest_sha256': digest(raw_manifest), 'source': note, 'archives': changes,
             'added_archives': list(add_archives), 'added_files': list(add_files),
             'files': config_changes, 'supersedes_colour_match': sorted(set(changes) & matched),
             'new_art_replacing_original_fallbacks': [':'.join(m) for m in sorted(confirmed)],
             'note': 'Updated archives record the source archive as reviewed_zip_sha256; member bytes are '
                     'authoritative. Earlier colour_match records of the archives listed in '
                     'supersedes_colour_match describe their previous members.'}
    if publication_identity is not None:
        entry['previous_publication_identity'] = previous_identity
        entry['publication_identity'] = {key: manifest[key] for key in previous_identity}
    manifest.setdefault('archive_updates', []).append(entry)
    after_objects = sprite_objects(manifest, after_recipes)
    (assets / 'manifest.json').write_bytes(encode_json(manifest))
    for sha in sorted(before_objects - after_objects):
        object_path(assets, sha).unlink()
    for directory in (assets / 'objects').iterdir():
        if directory.is_dir() and not any(directory.iterdir()):
            directory.rmdir()
    stored = {p.stem for p in (assets / 'objects').rglob('*.z')}
    if stored != after_objects:
        raise AssetError('Sprite object store differs from the manifest references')
    provenance_path = assets / 'provenance.json'
    if provenance_path.exists():
        provenance = parse_json(read_regular(provenance_path), 'provenance.json')
        provenance.setdefault('archive_updates', []).append({
            'previous_publication_manifest_sha256': digest(raw_manifest), 'source': note,
            'archives': len(changes), 'members': sum(c['changed_members'] for c in changes.values()),
            'changed_by_source': dict(sum((collections.Counter(c['changed_by_source']) for c in changes.values()),
                                          collections.Counter())),
            'files': sorted(config_changes), 'original_pixels_published': 0})
        if publication_identity is not None:
            provenance['archive_updates'][-1].update(
                previous_publication_identity=previous_identity,
                publication_identity={key: manifest[key] for key in previous_identity})
        provenance['publication_manifest_sha256'] = digest(encode_json(manifest))
        provenance_path.write_bytes(encode_json(provenance).replace(b'\n', b'\r\n'))
    return {'archives': len(changes), 'files': len(config_changes),
            'members': sum(c['changed_members'] for c in changes.values()),
            'objects_added': len(after_objects - before_objects), 'objects_removed': len(before_objects - after_objects),
            'manifest_sha256': digest(encode_json(manifest))}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--assets', type=Path, default=REPO / 'assets/remaster-sprites')
    parser.add_argument('--source', type=Path, required=True,
                        help='directory holding the replacement files at their bundle paths (fxdata/, creatrs/)')
    parser.add_argument('--data', type=Path, required=True, help="the player's original KeeperFX data directory")
    parser.add_argument('--archive', action='append', default=[], help='bundle archive to replace, e.g. fxdata/troll-sprites.zip')
    parser.add_argument('--file', action='append', default=[], help='bundle configuration file to replace')
    parser.add_argument('--add-archive', action='append', default=[], help='new fxdata/*.zip output; cannot replace an existing file')
    parser.add_argument('--add-file', action='append', default=[], help='new fxdata/ or creatrs/ .cfg/.toml output')
    parser.add_argument('--publication-identity', choices=sorted(PUBLICATION_IDENTITIES),
                        help='reviewed selection identity; preserves the previous identity in update history')
    parser.add_argument('--new-art', action='append', default=[], metavar='ARCHIVE:MEMBER',
                        help='confirm that a member previously rebuilt from original data is now new enhanced art')
    parser.add_argument('--note', required=True, help='where the replacement files come from, recorded in the manifest')
    args = parser.parse_args(argv)
    if not any((args.archive, args.file, args.add_archive, args.add_file)):
        parser.error('select at least one replacement or addition')
    if any(':' not in item for item in args.new_art):
        parser.error('--new-art takes ARCHIVE:MEMBER')
    try:
        result = update(args.assets.resolve(), args.source.resolve(), args.data.resolve(),
                        args.archive, args.file, args.note,
                        [tuple(item.split(':', 1)) for item in args.new_art],
                        add_archives=args.add_archive, add_files=args.add_file,
                        publication_identity=args.publication_identity)
    except (AssetError, OSError, KeyError) as error:
        parser.exit(1, f'Archive update: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
