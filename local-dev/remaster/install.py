# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the reviewed remaster from lossless publication recipes.

Original game files remain local. Native frontend replacements are activated only
for the launched session; the launcher restores originals and mod order on exit.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import sys
import uuid

REPO = Path(__file__).resolve().parents[2]
SELECTION = '9a285bacf69a60ea22bd0974c915ed37f1d5ab1b93bf20b7a0bc93390b0065de'
MOD = 'keeper_remaster_default'

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''): h.update(chunk)
    return h.hexdigest()

def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    created = False
    try:
        with temporary.open('x', encoding='utf-8', newline='\n') as f:
            created = True
            f.write(json.dumps(value, indent=2) + '\n'); f.flush(); os.fsync(f.fileno())
        os.replace(temporary, path)
    finally:
        if created and temporary.exists(): temporary.unlink()

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    import sys
    sys.path.insert(0, str(path.parent))
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result

def relative(value):
    p = PurePosixPath(value)
    if not isinstance(value, str) or '\\' in value or ':' in value or p.is_absolute() or not p.parts or any(x in ('.', '..') for x in p.parts):
        raise ValueError('Unsafe relative output: ' + str(value))
    return p

def target(root, name, create_control=True):
    """Resolve explicitly selected top-level data junctions, not descendant links."""
    p = relative(name)
    selected = root / p.parts[0]
    if p.parts[0] == 'remaster' and (selected.exists() or selected.is_symlink()) and (getattr(selected.lstat(), 'st_file_attributes', 0) & 0x400 or selected.is_symlink()):
        raise ValueError('Linked remaster control directory')
    top = selected.resolve()
    if not top.exists():
        if p.parts[0] != 'remaster': raise ValueError('Missing runtime directory: ' + str(top))
        if create_control: top.mkdir()
    current = top
    for part in p.parts[1:]:
        current = current / part
        if (current.exists() or current.is_symlink()) and (getattr(current.lstat(), 'st_file_attributes', 0) & 0x400 or current.is_symlink()):
            raise ValueError('Linked output descendant: ' + str(current))
    if not current.resolve().is_relative_to(top): raise ValueError('Output leaves selected root')
    return current

def assert_closed(powershell):
    result = subprocess.run([powershell, '-NoProfile', '-Command',
        "if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) { exit 9 }"], check=False)
    if result.returncode: raise RuntimeError('Close KeeperFX before installation; no game was stopped.')

def inventory(root):
    return [{'path': p.relative_to(root).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
            for p in sorted(root.rglob('*')) if p.is_file()]

def assemble(game, stage, powershell):
    sprites = module('remaster_sprite_import', REPO / 'tools/import_remaster_sprites.py')
    ui = module('remaster_ui_import', REPO / 'local-dev/world-remaster/frontend-art/ui_publication.py')
    sprite_result = sprites.build(REPO / 'assets/remaster-sprites', game / 'data', stage / 'sprites')
    if sprite_result['authority_manifest_sha256'] != SELECTION:
        raise ValueError('Sprite selection differs from accepted bundle')
    ui_manifest = json.loads((REPO / 'assets/remaster-ui/manifest.json').read_text(encoding='utf-8-sig'))
    # The prepared x64 runtime deliberately shares original banks through top-level
    # junctions. Snapshot only the manifest-bound originals into a private ordinary
    # tree so the UI decoder can keep its strict no-escape input-root contract.
    native_inputs = stage / 'native-inputs'
    for name, entry in ui_manifest['sources'].items():
        source = target(game, name, create_control=False)
        if source.stat().st_size != entry['bytes'] or sha(source) != entry['sha256']:
            raise ValueError('Original UI source mismatch: ' + name)
        destination = native_inputs / relative(name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        if sha(destination) != entry['sha256']: raise ValueError('Original UI source changed while staging')
    ui_result = ui.build_ui_assets(REPO / 'assets/remaster-ui', native_inputs, stage / 'frontend')
    if ui_result['selection'] != SELECTION: raise ValueError('UI selection differs from accepted bundle')
    terrain_stage = stage / 'terrain'
    for part in ('fxdata', 'mods'): (terrain_stage / part).mkdir(parents=True)
    subprocess.run([powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(REPO / 'local-dev/terrain/install_enhanced_terrain.ps1'), '-RuntimePath', str(terrain_stage),
        '-OriginalDataPath', str(game / 'data'), '-AssetsPath', str(REPO / 'assets/terrain/standard-hd')], check=True)
    package = 'remaster/packages/' + SELECTION
    changes = {}
    for entry in inventory(stage / 'sprites'):
        changes['mods/' + MOD + '/' + entry['path']] = stage / 'sprites' / entry['path']
    overlays = []
    native = {e['output']: e for e in ui_manifest['native']}
    for entry in ui_result['files']:
        rel = entry['path']
        if rel.startswith('ui/'):
            changes[package + '/' + rel] = stage / 'frontend' / rel
        else:
            if rel not in native: raise ValueError('Unexpected frontend output')
            changes[package + '/native/' + rel] = stage / 'frontend' / rel
            original = ui_manifest['sources'][native[rel]['source']]['sha256']
            overlays.append({'file': rel, 'source': package + '/native/' + rel,
                             'sha256': entry['sha256'], 'original_sha256': original})
    for entry in inventory(terrain_stage / 'fxdata'):
        changes['fxdata/' + entry['path']] = terrain_stage / 'fxdata' / entry['path']
    audio = REPO / 'config/mods/dk_modern_audio'
    for entry in inventory(audio): changes['mods/dk_modern_audio/' + entry['path']] = audio / entry['path']
    # Keep the installation-family ID stable so an owned V13 install upgrades
    # through the existing backup transaction; selection identifies its artwork.
    profile = {'schema': 1, 'id': 'keeperfx-remaster-v13', 'selection': SELECTION,
               'mod': MOD, 'ui_directory': package + '/ui', 'text_percent': 80,
               'lighting': True, 'emissive': 'source-v1', 'api_enabled': False,
               'overlays': overlays, 'files': [dict(path=k, sha256=sha(v), bytes=v.stat().st_size)
                                              for k, v in sorted(changes.items())]}
    dump(stage / 'default.json', profile)
    changes['remaster/default.json'] = stage / 'default.json'
    dump(stage / 'assembly-receipt.json', {'schema': 1, 'selection': SELECTION,
         'sprites': sprite_result, 'ui': ui_result, 'files': len(changes),
         'decoded_bytes': sum(p.stat().st_size for p in changes.values())})
    return profile, changes

def install(game, stage, powershell, verify_only=False):
    profile, changes = assemble(game, stage, powershell)
    if verify_only:
        for name, source in changes.items():
            destination = target(game, name, create_control=False)
            if not destination.is_file() or sha(destination) != sha(source):
                raise ValueError('Installed file mismatch: ' + name)
        return {'verified': True, 'selection': SELECTION, 'files': len(changes)}
    existing_profile = target(game, 'remaster/default.json')
    previous = json.loads(existing_profile.read_text()) if existing_profile.exists() else None
    mod_root = target(game, 'mods/' + MOD)
    if mod_root.exists() and (not previous or previous.get('id') != profile['id']):
        raise ValueError('An unrecognized remaster mod already exists; it was not replaced.')
    # No unrelated files may be silently hidden by a newly installed profile.
    if mod_root.exists():
        known = {x['path'] for x in previous['files'] if x['path'].startswith('mods/' + MOD + '/')}
        for item in inventory(mod_root):
            if 'mods/' + MOD + '/' + item['path'] not in known:
                raise ValueError('Unrecognized file in existing remaster mod: ' + item['path'])
    stamp = uuid.uuid4().hex
    backup = target(game, 'remaster/backups/' + stamp)
    backup.mkdir(parents=True)
    journal = {'schema': 1, 'state': 'prepared', 'entries': []}
    for name, source in changes.items():
        destination = target(game, name)
        before = sha(destination) if destination.exists() else None
        after = sha(source)
        if before == after: continue
        entry = {'path': name, 'before': before, 'after': after, 'backup': str(len(journal['entries']))}
        if before is not None: shutil.copyfile(destination, backup / entry['backup'])
        journal['entries'].append(entry)
    dump(backup / 'journal.json', journal)
    committed = []
    temporary_files = []
    assert_closed(powershell)
    try:
        for entry in journal['entries']:
            destination = target(game, entry['path'])
            if (sha(destination) if destination.exists() else None) != entry['before']:
                raise ValueError('Runtime changed during installation: ' + entry['path'])
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(destination.name + '.' + stamp + '.tmp')
            with temporary.open('xb') as f:
                temporary_files.append(temporary)
                with changes[entry['path']].open('rb') as s: shutil.copyfileobj(s, f)
            if sha(temporary) != entry['after']: raise ValueError('Staged copy mismatch')
            os.replace(temporary, destination)
            committed.append(entry)
        journal['state'] = 'installed'
    except BaseException:
        for entry in reversed(journal['entries']):
            destination = target(game, entry['path'])
            actual = sha(destination) if destination.exists() else None
            if actual == entry['before']: continue
            if actual != entry['after']: raise RuntimeError('Recovery needs unchanged files; backup retained: ' + str(backup))
            if entry['before'] is None: destination.unlink()
            else:
                saved = backup / entry['backup']
                if sha(saved) != entry['before']: raise RuntimeError('Backup hash mismatch; recovery retained.')
                temporary = destination.with_name(destination.name + '.' + stamp + '.restore.tmp')
                with temporary.open('xb') as f:
                    temporary_files.append(temporary)
                    with saved.open('rb') as s: shutil.copyfileobj(s, f)
                if sha(temporary) != entry['before']: raise RuntimeError('Recovery copy mismatch')
                os.replace(temporary, destination)
        journal['state'] = 'rolled_back'
        raise
    finally:
        try:
            dump(backup / 'journal.json', journal)
        finally:
            allowed_parents = {target(game, e['path']).parent for e in journal['entries']}
            for temporary in temporary_files:
                if temporary.exists():
                    if temporary.parent not in allowed_parents or stamp not in temporary.name or temporary.is_symlink():
                        raise RuntimeError('Unexpected temporary cleanup path; backup retained.')
                    temporary.unlink()
    return {'installed': True, 'selection': SELECTION, 'files': len(changes), 'changed': len(committed), 'backup': str(backup)}

def main():
    if sys.version_info < (3, 11): raise SystemExit('Python 3.11 or newer is required.')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--powershell', default='powershell.exe')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--stage-only', type=Path)
    args = parser.parse_args()
    game = args.runtime.resolve(strict=True)
    assert_closed(args.powershell)
    if args.stage_only:
        args.stage_only.mkdir(parents=True, exist_ok=False)
        profile, changes = assemble(game, args.stage_only.resolve(), args.powershell)
        print(json.dumps({'staged': True, 'selection': profile['selection'], 'files': len(changes)}))
        return
    control = target(game, 'remaster/operation.lock', create_control=not args.verify_only)
    lock = control.open('x')
    try:
        lock.write('installation pid ' + str(os.getpid())); lock.flush()
        with tempfile.TemporaryDirectory(prefix='keeperfx-remaster-', dir=game.parent) as folder:
            print(json.dumps(install(game, Path(folder), args.powershell, args.verify_only), indent=2))
    finally:
        lock.close()
        control.unlink()

if __name__ == '__main__': main()
