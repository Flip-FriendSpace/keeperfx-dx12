# SPDX-License-Identifier: GPL-2.0-or-later
"""Freeze a declared asset subset into a new opt-in review version; never deploy.

Usage: python local-dev/fidelity/package-review.py plan.json --verify-plan
       python local-dev/fidelity/package-review.py plan.json
Inputs and evidence are workspace-relative, SHA256 pinned. Only HD and native
frontend changes are supported. Original and build/shader bytes stay exact.
"""
from pathlib import Path, PurePosixPath
import argparse, copy, hashlib, json, os, re, shutil, stat, sys

DERIVED = {'root', 'manifest_sha256', 'mod_name', 'original_files', 'native_overlays'}
GROUPS = ('HD', 'Original', 'native-frontend', 'build')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def manifest_digest(report):
    return hashlib.sha256(canonical({k: v for k, v in report.items() if k not in DERIVED})).hexdigest()


def read_json(path):
    def pairs(values):
        result = {}
        for key, value in values:
            require(key not in result, 'Duplicate JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding='utf-8-sig'), object_pairs_hook=pairs)


def hash_value(value):
    require(isinstance(value, str) and re.fullmatch(r'[a-f0-9]{64}', value), 'Invalid SHA256')
    return value


def relative(value):
    require(isinstance(value, str) and value, 'Missing relative path')
    require('\\' not in value and ':' not in value and '\x00' not in value, 'Unsafe path: ' + value)
    p = PurePosixPath(value)
    require(p.parts and not p.is_absolute() and p.as_posix() == value, 'Noncanonical relative path: ' + value)
    require(all(part not in ('.', '..') and not part.endswith((' ', '.')) for part in p.parts), 'Unsafe path component: ' + value)
    reserved = {'con', 'prn', 'aux', 'nul', *(f'com{n}' for n in range(1, 10)), *(f'lpt{n}' for n in range(1, 10))}
    require(all(part.split('.')[0].casefold() not in reserved for part in p.parts), 'Windows reserved path: ' + value)
    return p


def no_links(path, boundary):
    path, boundary = Path(path).absolute(), Path(boundary).absolute()
    require(path.is_relative_to(boundary), 'Path outside workspace: ' + str(path))
    current = boundary
    for part in (None, *path.relative_to(boundary).parts):
        if part is not None:
            current = current / part
        if not os.path.lexists(current):
            continue
        info = current.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not (getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)), 'Linked/reparse path refused: ' + str(current))
    require(path.resolve().is_relative_to(boundary.resolve()), 'Resolved path escapes workspace')
    return path


def local(workspace, value):
    return no_links(Path(workspace) / Path(relative(value)), workspace)


def pinned(workspace, record):
    path = local(workspace, record['source'])
    require(path.is_file(), 'Missing source file: ' + str(path))
    require(digest(path) == hash_value(record['sha256']), 'Source hash mismatch: ' + str(path))
    if 'bytes' in record:
        require(path.stat().st_size == record['bytes'], 'Source size mismatch: ' + str(path))
    return path


def absolute_local(workspace, value):
    path = Path(value)
    require(path.is_absolute(), 'Manifest source must be absolute: ' + str(path))
    return no_links(path, workspace)


def indexed(entries):
    result, seen = {}, set()
    for item in entries:
        name = relative(item['file']).as_posix()
        require(name.casefold() not in seen and name.casefold() != 'integration-owner.json', 'Duplicate/reserved member: ' + name)
        seen.add(name.casefold())
        hash_value(item['sha256'])
        result[name] = copy.deepcopy(item)
    return result


def checked_tree(workspace, root, entries, manifest, variant, owner_required=False):
    root = absolute_local(workspace, str(root))
    require(root.is_dir(), 'Missing package directory: ' + str(root))
    expected = indexed(entries)
    actual = set()
    for p in root.rglob('*'):
        no_links(p, workspace)
        require(p.is_file() or p.is_dir(), 'Unsupported package member')
        if p.is_file():
            actual.add(p.relative_to(root).as_posix())
    owner = root / 'integration-owner.json'
    allowed = set(expected)
    if owner.exists():
        mark = read_json(owner)
        require(mark.get('owner') == 'remaster-integration' and mark.get('manifest_sha256') == manifest and mark.get('variant') == variant, 'Owner/version/variant mismatch: ' + str(root))
        require(indexed(mark['files']) == expected, 'Owner member list mismatch: ' + str(root))
        allowed.add('integration-owner.json')
    else:
        require(not owner_required, 'Missing package owner: ' + str(root))
    require(actual == allowed, 'Package members differ: ' + str(root))
    for name, item in expected.items():
        path = root / name
        require(digest(path) == item['sha256'], 'Package hash mismatch: ' + str(path))
        if 'bytes' in item:
            require(path.stat().st_size == item['bytes'], 'Package size mismatch: ' + str(path))
    return expected




def runtime_control_path(workspace, runtime, path):
    """Read only: permit the four established same-workspace runtime junctions."""
    runtime = no_links(runtime, workspace)
    path = Path(path).absolute()
    require(path.is_relative_to(runtime), 'Runtime control outside expected runtime')
    rel = relative(path.relative_to(runtime).as_posix())
    first = runtime / rel.parts[0]
    if not os.path.lexists(first):
        return no_links(path, workspace)
    info = first.lstat()
    if not (getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)):
        return no_links(path, workspace)
    require(rel.parts[0] in ('campgns', 'ldata', 'data', 'mods') and getattr(info, 'st_reparse_tag', None) == 0xA0000003 and first.is_dir(), 'Unapproved runtime junction')
    expected = no_links(Path(workspace) / 'runtime/keeperfx' / rel.parts[0], workspace)
    require(first.resolve() == expected.resolve(), 'Runtime junction has unexpected target')
    resolved = no_links(expected.joinpath(*rel.parts[1:]), workspace)
    require(path.resolve() == resolved.resolve(), 'Runtime junction suffix differs')
    return resolved


def verify_native_control(workspace, runtime, name, control):
    require(re.fullmatch(r'(ldata/front\.raw|data/(loading|gmap)(32|64)\.raw|campgns/[a-z0-9_-]+/(rgmap[0-9]+\.raw|viframe[0-9]+\.dat))', name), 'Unsupported native frontend destination: ' + name)
    require(set(control) == {'original_sha256', 'palette'}, 'Native baseline contract fields differ')
    require(digest(runtime_control_path(workspace, runtime, runtime / name)) == hash_value(control['original_sha256']), 'Runtime native baseline differs: ' + name)
    palette = control['palette']
    require(set(palette) == {'path', 'expected_sha256', 'current_sha256', 'replace_palette'}, 'Native palette contract fields differ')
    require(palette['replace_palette'] is False and palette['current_sha256'] == palette['expected_sha256'], 'Palette replacement unsupported')
    palette_path = Path(palette['path'])
    require(palette_path.is_absolute() and palette_path.is_relative_to(runtime) and palette_path.suffix == '.pal', 'Palette must be an unchanged runtime palette')
    require(digest(runtime_control_path(workspace, runtime, palette_path)) == hash_value(palette['expected_sha256']), 'Native palette changed')


def verify_plan(workspace, plan_path):
    workspace = Path(workspace).absolute()
    plan_path = no_links(Path(plan_path).absolute(), workspace)
    plan = read_json(plan_path)
    require(plan.get('schema') == 1, 'Unsupported plan schema')
    version = plan['version']
    require(re.fullmatch(r'v[1-9][0-9]*', version), 'Invalid version')
    integration = local(workspace, 'artifacts/remaster-integration')
    base_path = pinned(workspace, plan['base_report'])
    match = re.fullmatch(r'combined-full-review-v([1-9][0-9]*)\.json', base_path.name)
    require(match and base_path.parent == integration and int(version[1:]) == int(match.group(1)) + 1, 'Base must be preceding versioned review report')
    base = read_json(base_path)
    require(base.get('schema') == 1 and base.get('local_only') is True, 'Not a local full review base')
    base_hash = hash_value(plan['base_report']['manifest_sha256'])
    require(base['manifest_sha256'] == base_hash, 'Wrong base manifest')
    base_root = integration / 'combined' / base_hash
    require(absolute_local(workspace, base['root']) == base_root / 'HD', 'Unexpected base root')
    require(base['mod_name'] == 'remaster_core_' + base_hash[:12], 'Unexpected base mod name')
    require(base['review_scope'].get('normal_api') is False, 'Normal API must remain disabled')
    original = [{k: v for k, v in e.items() if k != 'source'} for e in base['original_files']]
    native = [{k: v for k, v in e.items() if k != 'source'} for e in base['native_overlays']]
    require(original == base['original_control_spec'] and native == base['native_overlay_spec'], 'Derived source specs disagree')
    for variant, key in [('Original', 'original_files'), ('native-frontend', 'native_overlays')]:
        for e in base[key]:
            require(absolute_local(workspace, e['source']) == base_root / variant / e['file'], 'Derived member source escapes its variant')
    scope = base['review_scope']
    require(scope.get('build_artifact_dir') and scope.get('build_files'), 'Base needs a pinned build snapshot')
    build_root = absolute_local(workspace, scope['build_artifact_dir'])
    require(build_root.is_relative_to(integration / 'builds'), 'Build source outside integration/builds')
    roots = {'HD': base_root / 'HD', 'Original': base_root / 'Original', 'native-frontend': base_root / 'native-frontend', 'build': build_root}
    lists = {'HD': base['files'], 'Original': base['original_control_spec'], 'native-frontend': base['native_overlay_spec'], 'build': scope['build_files']}
    maps = {v: checked_tree(workspace, roots[v], lists[v], base_hash, v, v in ('HD', 'Original')) for v in GROUPS}
    build = maps['build']
    require({'keeperfx-dx12.exe', 'keeperfx-local.exe'}.issubset(build), 'Incomplete executable pair')
    require(all(re.fullmatch(r'(keeperfx-(dx12|local)\.exe|shaders/[a-z0-9.-]+\.dxil)', name) for name in build), 'Unexpected build member')
    require(build['keeperfx-dx12.exe']['sha256'] == scope['required_dx12_executable_sha256'] and build['keeperfx-local.exe']['sha256'] == scope['default_executable_sha256'], 'Build executable pins disagree')
    shader_spec = [{'file': n.removeprefix('shaders/'), 'sha256': e['sha256'], 'bytes': e['bytes']} for n, e in sorted(build.items()) if n.startswith('shaders/')]
    require(shader_spec == scope['shader_files'], 'Shader specs disagree with build snapshot')
    runtime = local(workspace, 'runtime/keeperfx-x64')
    for name, entry in build.items():
        path = no_links(runtime / name, workspace)
        require(digest(path) == hash_value(entry['base_sha256']), 'Runtime build baseline differs: ' + name)
    for e in maps['native-frontend'].values():
        verify_native_control(workspace, runtime, e['file'], {k: e[k] for k in ('original_sha256', 'palette')})
    evidence = plan.get('evidence', [])
    require(evidence, 'Pinned review evidence is required')
    for item in evidence:
        pinned(workspace, item)
    require(plan.get('review', {}).get('user_appearance_accepted') is False, 'Builder cannot declare user appearance acceptance')
    require(plan['review'].get('summary') and isinstance(plan['review'].get('included'), list) and isinstance(plan['review'].get('pending'), list), 'Explicit review scope and pending limits required')
    changes = plan.get('changes', [])
    require(changes, 'Empty change subset')
    output_maps = copy.deepcopy(maps)
    sources = {(v, n): roots[v] / n for v in GROUPS for n in maps[v]}
    seen = set()
    for change in changes:
        variant = change['variant']
        require(variant in ('HD', 'native-frontend'), 'Original and build changes are forbidden')
        name = relative(change['file']).as_posix()
        require(name.casefold() != 'integration-owner.json', 'Cannot replace ownership marker')
        key = (variant, name.casefold())
        require(key not in seen, 'Duplicate replacement target')
        seen.add(key)
        require(not any(n.casefold() == name.casefold() and n != name for n in maps[variant]), 'Case-colliding destination')
        old = maps[variant].get(name)
        require('before_sha256' in change, 'Explicit before_sha256 or null required')
        require(change['before_sha256'] == (old['sha256'] if old else None), 'Declared before hash mismatch: ' + name)
        source = pinned(workspace, change)
        require(old is None or old['sha256'] != change['sha256'], 'Declared replacement is unchanged')
        entry = copy.deepcopy(old) if old else {'file': name}
        if variant == 'native-frontend':
            control = change.get('native_control')
            if old:
                expected_control = {k: old[k] for k in ('original_sha256', 'palette')}
                require(control is None or control == expected_control, 'Existing native baseline contract must remain exact')
                control = expected_control
            require(isinstance(control, dict), 'New native overlay requires native_control')
            verify_native_control(workspace, runtime, name, control)
            entry.update(copy.deepcopy(control))
        else:
            require('native_control' not in change, 'Native control on HD change is unsupported')
        entry.update(sha256=change['sha256'], bytes=source.stat().st_size)
        output_maps[variant][name] = entry
        sources[(variant, name)] = source
    declared = {(c['variant'], c['file']) for c in changes}
    difference = {(v, n) for v in GROUPS for n in output_maps[v] if n not in maps[v] or maps[v][n]['sha256'] != output_maps[v][n]['sha256']}
    require(difference == declared, 'Unexpected change outside declared subset')
    report = {k: copy.deepcopy(v) for k, v in base.items() if k not in DERIVED}
    report['kind'] = 'faithful-restoration-opt-in-review-' + version
    report['previous_manifest'] = base_hash
    report['files'] = [output_maps['HD'][n] for n in sorted(output_maps['HD'])]
    report['original_control_spec'] = copy.deepcopy(base['original_control_spec'])
    report['native_overlay_spec'] = [output_maps['native-frontend'][n] for n in sorted(output_maps['native-frontend'])]
    report['historical_base_descriptors'] = {'manifest_sha256': base_hash, **{k: copy.deepcopy(base.get(k)) for k in ('coverage', 'excluded_pending', 'preserve')}, 'runtime': base['review_scope'].get('runtime')}
    report['excluded_pending'] = copy.deepcopy(plan['review']['pending'])
    report['coverage'] = {'package_members': {v: len(output_maps[v]) for v in GROUPS}, 'frontend_native_resources': len(output_maps['native-frontend']), 'declared_changed_members': len(changes), 'reviewed_scope': copy.deepcopy(plan['review']['included'])}
    report['preserve'] = {'undeclared_asset_bytes': 'identical to pinned base report', 'Original_and_build_and_shaders': 'byte-identical to pinned base report', 'native_palettes': 'original runtime bytes; no palette replacement', 'default_selection': 'unchanged', 'candidate_HD_files': {n: e['sha256'] for n, e in sorted(output_maps['HD'].items()) if ('HD', n) in declared}}
    report['review_scope']['runtime'] = 'Executables and shaders are frozen byte for byte from the pinned base. Only declared HD/native frontend assets differ. Component receipts are pinned in restoration_review; exact combined bundle runtime review remains pending.'
    report['review_scope']['user_acceptance'] = 'Pending user appearance acceptance for this exact opt-in review bundle.'
    frozen_build = integration / 'builds' / ('review-' + version)
    report['review_scope']['build_artifact_dir'] = str(frozen_build)
    report['restoration_review'] = {'version': version, 'base_report': copy.deepcopy(plan['base_report']), 'plan': {'source': plan_path.relative_to(workspace).as_posix(), 'sha256': digest(plan_path)}, 'builder_sha256': digest(__file__), 'declared_changes': copy.deepcopy(changes), 'evidence': copy.deepcopy(evidence), 'review': copy.deepcopy(plan['review']), 'default_selection_changed': False, 'runtime_changed': False, 'whole_bundle_runtime': 'pending_exact_new_package_review'}
    manifest = manifest_digest(report)
    dest = integration / 'combined' / manifest
    report.update(root=str(dest / 'HD'), manifest_sha256=manifest, mod_name='remaster_core_' + manifest[:12], original_files=[dict(e, source=str(dest / 'Original' / e['file'])) for e in report['original_control_spec']], native_overlays=[dict(e, source=str(dest / 'native-frontend' / e['file'])) for e in report['native_overlay_spec']])
    require(manifest_digest(report) == manifest, 'Canonical manifest instability')
    output_paths = [dest, frozen_build, integration / ('combined-full-review-' + version + '.json'), integration / ('combined-full-runtime-selection-' + version + '.json'), integration / ('combined-full-package-proof-' + version + '.json')]
    for path in output_paths:
        no_links(path, workspace)
        require(not os.path.lexists(path), 'Refusing existing output: ' + str(path))
    return {'workspace': workspace, 'plan_path': plan_path, 'plan': plan, 'base_path': base_path, 'base': base, 'base_maps': maps, 'maps': output_maps, 'sources': sources, 'report': report, 'dest': dest, 'build_dest': frozen_build, 'integration': integration, 'outputs': output_paths, 'declared': declared}


def write_new(path, value):
    with Path(path).open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def copy_checked(workspace, source, target, expected):
    no_links(source, workspace)
    require(digest(source) == expected, 'Source changed after preflight: ' + str(source))
    no_links(target, workspace)
    target.parent.mkdir(parents=True, exist_ok=True)
    no_links(target, workspace)
    with source.open('rb') as src, target.open('xb') as dst:
        shutil.copyfileobj(src, dst)
    require(digest(target) == expected, 'Copied output mismatch: ' + str(target))


def build_package(state):
    workspace, report = state['workspace'], state['report']
    dest, build_dest = state['dest'], state['build_dest']
    # Every source has already passed the complete preflight. Reserve fresh roots.
    dest.mkdir()
    journal = dest / 'package-review-state.jsonl'
    def record(status, **extra):
        with journal.open('a', encoding='utf-8', newline='\n') as stream:
            stream.write(json.dumps({'owner': 'remaster-integration', 'status': status, 'manifest_sha256': report['manifest_sha256'], **extra}) + '\n')
    record('building', plan=str(state['plan_path']), outputs=[str(p) for p in state['outputs']])
    try:
        build_dest.mkdir()
        roots = {v: (build_dest if v == 'build' else dest / v) for v in GROUPS}
        for variant in GROUPS:
            roots[variant].mkdir(exist_ok=True)
            entries = state['maps'][variant]
            for name, entry in entries.items():
                copy_checked(workspace, state['sources'][(variant, name)], roots[variant] / name, entry['sha256'])
            write_new(roots[variant] / 'integration-owner.json', {'owner': 'remaster-integration', 'manifest_sha256': report['manifest_sha256'], 'variant': variant, 'files': [entries[n] for n in sorted(entries)]})
            checked_tree(workspace, roots[variant], list(entries.values()), report['manifest_sha256'], variant, True)
        require(state['maps']['Original'] == state['base_maps']['Original'] and state['maps']['build'] == state['base_maps']['build'], 'Protected Original/build metadata changed')
        for entry in state['plan']['evidence']:
            pinned(workspace, entry)
        copy_checked(workspace, state['base_path'], dest / 'base-report.json', state['plan']['base_report']['sha256'])
        copy_checked(workspace, state['plan_path'], dest / 'review-plan.json', report['restoration_review']['plan']['sha256'])
        selection = {'world_manifest': report['manifest_sha256'], 'variants': {v: {'root': str(dest / v), 'files': report['files'] if v == 'HD' else report['original_control_spec'], 'sha256': hashlib.sha256(canonical(report['files'] if v == 'HD' else report['original_control_spec'])).hexdigest()} for v in ('HD', 'Original')}, 'scope': state['plan']['review']['summary'], 'default_selection_changed': False}
        proof = {'schema': 1, 'status': 'packaged_verified_not_deployed', 'manifest_sha256': report['manifest_sha256'], 'previous_manifest': state['base']['manifest_sha256'], 'declared_changes': state['plan']['changes'], 'all_other_asset_bytes_preserved': True, 'original_and_build_bytes_identical': True, 'default_selection_changed': False, 'runtime_changed': False, 'whole_bundle_runtime': 'pending_exact_new_package_review', 'plan_sha256': report['restoration_review']['plan']['sha256'], 'base_report_sha256': state['plan']['base_report']['sha256'], 'counts': {v: len(state['maps'][v]) for v in GROUPS}}
        write_new(dest / 'integration-owner.json', {'owner': 'remaster-integration', 'manifest_sha256': report['manifest_sha256'], 'variant': 'bundle', 'review_version': state['plan']['version'], 'build_root': str(build_dest)})
        for name, doc in [('combined-full-review.json', report), ('combined-full-runtime-selection.json', selection), ('package-review-proof.json', proof)]:
            write_new(dest / name, doc)
        version, integration = state['plan']['version'], state['integration']
        write_new(integration / ('combined-full-runtime-selection-' + version + '.json'), selection)
        write_new(integration / ('combined-full-package-proof-' + version + '.json'), proof)
        record('verified_awaiting_versioned_report')
        # The launcher-visible versioned report is published last. No default pointer is written.
        write_new(integration / ('combined-full-review-' + version + '.json'), report)
        record('complete')
        return proof
    except Exception as error:
        record('failed_preserved_for_inspection', error=str(error))
        raise


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('plan', type=Path)
    ap.add_argument('--workspace', type=Path, required=True,
                    help='Workspace containing artifacts and runtime; never inferred from this checkout')
    ap.add_argument('--verify-plan', action='store_true')
    args = ap.parse_args()
    state = verify_plan(args.workspace, args.plan)
    if args.verify_plan:
        result = {'status': 'plan_verified_no_writes', 'manifest_sha256': state['report']['manifest_sha256'], 'outputs': [str(p) for p in state['outputs']], 'declared_changes': sorted([list(p) for p in state['declared']]), 'counts': {v: len(state['maps'][v]) for v in GROUPS}}
    else:
        result = build_package(state)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        print('Review package refused: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
