# SPDX-License-Identifier: GPL-2.0-or-later
"""Source-bound first-person sprite candidates; never deploy or change active art.

All outputs are local commercial-data derivatives. The original archive members
remain byte-identical; only complete, reviewed FP actions may gain new members.
"""
from pathlib import Path
import argparse, ast, copy, hashlib, io, json, math, re, zipfile
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent

def digest(data):
    return hashlib.sha256(data).hexdigest()

def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))

def save_json(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

def source_manifest(workspace, manifest_path=None):
    path = manifest_path or workspace / 'artifacts/remaster-integration/combined-full-review.json'
    return path, read_json(path)

def checked_member(name):
    p = Path(name)
    if p.is_absolute() or '..' in p.parts or '\\' in name:
        raise ValueError('Unsafe archive member: ' + name)
    return name

def selected_entries(z, cfg):
    entries = json.loads(z.read('sprites.json'))
    names = set(re.findall(r'^\s*\w+\s*=\s*([A-Za-z_][\w]*)\s*$', cfg, re.M))
    return [e for e in entries if e['name'] in names]

def inventory(workspace, output, manifest_path=None):
    path, manifest = source_manifest(workspace, manifest_path)
    root = Path(manifest['root'])
    models = []
    for file in manifest['files']:
        if not re.fullmatch(r'creatrs/[\w]+\.cfg', file['file']):
            continue
        slug = Path(file['file']).stem
        cfg_path = root / file['file']
        cfg = cfg_path.read_text(encoding='utf-8-sig')
        assert digest(cfg_path.read_bytes()) == file['sha256']
        archive_name = 'fxdata/' + slug + '-sprites.zip'
        archive = root / archive_name
        if not archive.exists():
            continue
        expected = next(e['sha256'] for e in manifest['files'] if e['file'] == archive_name)
        assert digest(archive.read_bytes()) == expected
        with zipfile.ZipFile(archive) as z:
            entries = selected_entries(z, cfg)
            actions = []
            for e in entries:
                scale = e.get('fp_resolution_scale', e.get('resolution_scale', 1))
                actions.append({'name': e['name'], 'resolution_scale': scale,
                    'directions': len(e['fp']), 'frames': sum(map(len, e['fp'])),
                    'status': 'native_fallback' if scale == 1 else 'preserve_existing_enhanced'})
        models.append({'slug': slug, 'archive': str(archive), 'archive_sha256': expected,
            'config': str(cfg_path), 'config_sha256': digest(cfg_path.read_bytes()),
            'actions': actions})
    result = {'schema': 1, 'baseline_manifest': manifest['manifest_sha256'],
        'baseline_manifest_file_sha256': digest(path.read_bytes()), 'models': models,
        'view_contract': 'FP bank is selected only for creature control/passenger perspective; front strategy view uses TD.',
        'native_fp_frames': sum(a['frames'] for m in models for a in m['actions'] if a['resolution_scale'] == 1),
        'local_only': True}
    save_json(output / 'inventory.json', result)
    print(json.dumps({'models': len(models), 'native_fp_frames': result['native_fp_frames'], 'output': str(output)}))

def sheet(images, cols, rows, path, factor=8):
    cw, ch = max(i.width for i in images), max(i.height for i in images)
    factor = min(factor, max(1, 1800 // max(cw * cols, ch * rows)))
    result = Image.new('RGBA', (cw * cols * factor, ch * rows * factor))
    for n, im in enumerate(images):
        result.alpha_composite(im.resize((im.width * factor, im.height * factor), Image.Resampling.NEAREST),
            ((n % cols) * cw * factor, (n // cols) * ch * factor))
    path.parent.mkdir(parents=True, exist_ok=True)
    result.save(path)

def prepare(workspace, output, slug):
    inv = read_json(output / 'inventory.json')
    model = next(m for m in inv['models'] if m['slug'] == slug)
    out = output / slug
    if out.exists():
        raise ValueError('Refusing existing creature preparation: ' + str(out))
    archive = Path(model['archive'])
    assert digest(archive.read_bytes()) == model['archive_sha256']
    cfg = Path(model['config']).read_text(encoding='utf-8-sig')
    out.mkdir(parents=True)
    jobs, actions = [], []
    with zipfile.ZipFile(archive) as z:
        for entry in selected_entries(z, cfg):
            if entry.get('fp_resolution_scale', entry.get('resolution_scale', 1)) != 1:
                continue
            action = entry['name'].split('_HD_', 1)[-1].lower()
            record = {'action': action, 'entry_name': entry['name'], 'entry': entry, 'sequences': []}
            for d, frames in enumerate(entry['fp']):
                infos = []
                for n, frame in enumerate(frames):
                    raw = z.read(checked_member(frame['file']))
                    im = Image.open(io.BytesIO(raw)).convert('RGBA')
                    assert im.size == (frame['frame_w'], frame['frame_h'])
                    rel = f'references/{action}/fp/d{d}-{n:02}.png'
                    target = out / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(raw)
                    infos.append({'file': rel, 'frame_size': list(im.size),
                        'anchor': [-frame['offset_x'], -frame['offset_y']],
                        'original_member': frame['file'], 'sha256': digest(raw),
                        'metadata': frame})
                record['sequences'].append(infos)
            count = len(entry['fp'][0])
            groups = [list(range(len(entry['fp'])))] if count <= 2 else [[d] for d in range(len(entry['fp']))]
            for directions in groups:
                cells = [(d, n) for d in directions for n in range(len(entry['fp'][d]))]
                cols = 3 if len(cells) in (5, 6) else min(4, len(cells))
                rows = math.ceil(len(cells) / cols)
                job_id = action + '-' + ('all' if len(directions) > 1 else f'd{directions[0]}')
                originals = [Image.open(out / record['sequences'][d][n]['file']).convert('RGBA') for d, n in cells]
                sheet(originals, cols, rows, out / f'references/{job_id}-original.png')
                appearance = []
                for d in directions:
                    td = entry['td'][min(d, len(entry['td']) - 1)][0]
                    appearance.append(Image.open(io.BytesIO(z.read(td['file']))).convert('RGBA'))
                sheet(appearance, min(3, len(appearance)), math.ceil(len(appearance) / 3),
                    out / f'references/{job_id}-appearance.png', factor=2)
                jobs.append({'id': job_id, 'action': action, 'entry_name': entry['name'],
                    'cols': cols, 'rows': rows, 'cells': cells,
                    'reference': f'references/{job_id}-original.png',
                    'appearance': f'references/{job_id}-appearance.png',
                    'generated': f'generated/{job_id}-v1.png', 'status': 'not_generated'})
            actions.append(record)
    save_json(out / 'source.json', {'schema': 1, 'model': model, 'baseline_manifest': inv['baseline_manifest'], 'actions': actions})
    save_json(out / 'generation-plan.json', {'schema': 1, 'slug': slug, 'jobs': jobs,
        'policy': 'Original FP frames govern pose/camera/colours/anchors/timing. Existing TD art supplies only material and identity context. One transform per complete direction/action sequence. Preserve true generated alpha. No runtime installation.'})
    print(json.dumps({'slug': slug, 'actions': len(actions), 'jobs': len(jobs), 'frames': sum(len(s) for a in actions for s in a['sequences'])}))

def primitives(workspace, out):
    path = HERE.parent / 'package_imp_set.py'
    tree = ast.parse(path.read_text(encoding='utf-8-sig'))
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('split', 'bounds', 'register_sequence', 'engine_png')]
    palette = np.frombuffer((workspace / 'runtime/keeperfx-x64/data/png_conv_pal.dat').read_bytes(), dtype=np.uint8).reshape(256, 3)
    env = {'np': np, 'Image': Image, 'ImageFilter': ImageFilter, 'OUT': out, 'palette': palette,
        'palette_rgb': (palette.astype(np.uint16) * 4).astype(np.uint8)}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), env)
    return env

def authenticate_source(out, source):
    model = source['model']
    archive = Path(model['archive'])
    assert digest(archive.read_bytes()) == model['archive_sha256'], 'Changed baseline archive'
    config = Path(model['config'])
    assert digest(config.read_bytes()) == model['config_sha256'], 'Changed baseline config'
    with zipfile.ZipFile(archive) as z:
        entries = {e['name']: e for e in selected_entries(z, config.read_text(encoding='utf-8-sig'))}
        for action in source['actions']:
            entry = entries[action['entry_name']]
            assert entry == action['entry'], 'Source metadata differs from baseline'
            assert len(entry['fp']) == len(action['sequences']), 'Missing source direction'
            for frames, infos in zip(entry['fp'], action['sequences']):
                assert len(frames) == len(infos), 'Missing source frame'
                for frame, info in zip(frames, infos):
                    assert frame == info['metadata'], 'Changed source frame metadata'
                    assert frame['file'] == info['original_member'], 'Changed source member'
                    assert info['frame_size'] == [frame['frame_w'], frame['frame_h']], 'Changed source canvas'
                    assert info['anchor'] == [-frame['offset_x'], -frame['offset_y']], 'Changed source anchor'
                    path = out / info['file']
                    assert path.resolve().is_relative_to(out.resolve()), 'Source escapes candidate directory'
                    raw = path.read_bytes()
                    assert digest(raw) == info['sha256'] and raw == z.read(checked_member(frame['file'])), 'Changed original reference'


def action_jobs(plan, action):
    jobs = [j for j in plan['jobs'] if j['action'] == action['action']]
    expected = [(d, n) for d, sequence in enumerate(action['sequences']) for n in range(len(sequence))]
    actual = [tuple(c) for job in jobs for c in job['cells']]
    assert sorted(actual) == expected, 'Missing or duplicate action frame in generation plan'
    assert len({j['id'] for j in jobs}) == len(jobs), 'Duplicate generation job'
    return jobs


def process(workspace, output, slug, job_id, version='v1'):
    out = output / slug
    source = read_json(out / 'source.json')
    plan = read_json(out / 'generation-plan.json')
    authenticate_source(out, source)
    job = next(j for j in plan['jobs'] if j['id'] == job_id)
    action = next(a for a in source['actions'] if a['action'] == job['action'])
    action_jobs(plan, action)
    if slug == 'ghost':
        raise ValueError('Ghost requires its dedicated index-aware additive encoder')
    if not re.fullmatch(r'v[1-9][0-9]*', version): raise ValueError('Invalid candidate version')
    report_path = out / f'review/{job_id}-{version}-report.json'
    if report_path.exists(): raise ValueError('Refusing existing processed candidate revision')
    generated = out / f'generated/{job_id}-{version}.png'
    im = Image.open(generated).convert('RGBA')
    assert im.getchannel('A').getextrema()[0] == 0, 'Generated sheet has no real alpha'
    p = primitives(workspace, out)
    cells = p['split'](generated, job['cols'], job['rows'], required_cells=list(range(len(job['cells']))))
    frames, loops = [], []
    for d in sorted({c[0] for c in job['cells']}):
        indices = [i for i, c in enumerate(job['cells']) if c[0] == d]
        infos = action['sequences'][d]
        assert len(indices) == len(infos), 'A partial sequence cannot be independently fitted'
        registered = p['register_sequence']([cells[i] for i in indices], infos, out)
        for n, (image, info) in enumerate(zip(registered, infos)):
            path = out / f'frames/{job["action"]}/{version}/d{d}-{n:02}'
            path.parent.mkdir(parents=True, exist_ok=True)
            assert max(image.size) <= 1024, 'Sprite exceeds texture loader bound'
            encoded = p['engine_png'](image)
            native = encoded.resize(tuple(info['frame_size']), Image.Resampling.BOX)
            image.save(str(path) + '-rgba.png'); encoded.save(str(path) + '-hd.png'); native.save(str(path) + '-native.png')
            original = Image.open(out / info['file']).convert('RGBA')
            a = np.asarray(original)[:, :, 3] >= 64
            b = np.asarray(native)[:, :, 3] >= 64
            intersection = int(np.logical_and(a, b).sum()); union = int(np.logical_or(a, b).sum())
            bbox = p['bounds'](encoded)
            assert bbox and bbox[0] > 0 and bbox[1] > 0 and bbox[2] < encoded.width and bbox[3] < encoded.height, 'Clipped generated sprite'
            frames.append({'direction': d, 'frame': n, 'size': list(encoded.size), 'bounds': bbox,
                'original': info['file'], 'rgba': str(path.relative_to(out)) + '-rgba.png',
                'hd': str(path.relative_to(out)) + '-hd.png', 'native': str(path.relative_to(out)) + '-native.png',
                'sha256': digest(Path(str(path) + '-hd.png').read_bytes()),
                'rgba_sha256': digest(Path(str(path) + '-rgba.png').read_bytes()),
                'mask_iou_at_native': intersection / max(1, union), 'coverage_ratio': float(b.sum() / max(1, a.sum()))})
            pair = Image.new('RGB', (original.width * 8 + 16, original.height * 4 + 24), (39, 42, 43))
            draw = ImageDraw.Draw(pair); draw.text((4, 4), f'{job["action"]} d{d} frame{n}: original / candidate', fill='white')
            orig4 = original.resize((original.width * 4, original.height * 4), Image.Resampling.NEAREST)
            pair.paste(orig4, (0, 24), orig4); pair.paste(encoded, (original.width * 4 + 16, 24), encoded)
            loops.append(pair)
    review = out / f'review/{job_id}-{version}'
    review.parent.mkdir(parents=True, exist_ok=True)
    cw = max(i.width for i in loops); ch = max(i.height for i in loops)
    cols = min(3, len(loops)); board = Image.new('RGB', (cw * cols, ch * math.ceil(len(loops) / cols)), (39, 42, 43))
    for i, frame in enumerate(loops): board.paste(frame, ((i % cols) * cw, (i // cols) * ch))
    board.save(str(review) + '-contact.png')
    loops[0].save(str(review) + '-loop.gif', save_all=True, append_images=loops[1:], duration=150, loop=0)
    report = {'schema': 1, 'job': job_id, 'source_sha256': digest((out / 'source.json').read_bytes()),
        'generated': str(generated.relative_to(out)), 'generated_sha256': digest(generated.read_bytes()), 'frames': frames,
        'status': 'needs_visual_review', 'preview_timing': '150ms diagnostic stepping, not engine cadence qualification',
        'metrics_are_diagnostic_not_visual_acceptance': True}
    save_json(report_path, report)
    print(json.dumps({'job': job_id, 'frames': len(frames), 'contact': str(review) + '-contact.png',
        'min_iou': min(f['mask_iou_at_native'] for f in frames)}))

def package(workspace, output, slug, target):
    target = target.resolve()
    if slug == 'ghost': raise ValueError('Ghost packaging requires its dedicated index-aware encoder')
    out = output / slug; source = read_json(out / 'source.json'); plan = read_json(out / 'generation-plan.json')
    authenticate_source(out, source)
    original_path = Path(source['model']['archive'])
    assert digest(original_path.read_bytes()) == source['model']['archive_sha256']
    if target.exists(): raise ValueError('Refusing existing candidate archive')
    selected = read_json(out / 'selection.json')
    approved = selected['jobs']
    replacements = {}; completed = []
    with zipfile.ZipFile(original_path) as z:
        entries = json.loads(z.read('sprites.json')); original_entries = copy.deepcopy(entries)
        for action in source['actions']:
            jobs = action_jobs(plan, action)
            if not all(j['id'] in approved for j in jobs): continue
            entry = next(e for e in entries if e['name'] == action['entry_name'])
            for job in jobs:
                report_path = out / approved[job['id']].get('report', f'review/{job["id"]}-report.json')
                assert report_path.resolve().is_relative_to(out.resolve()), 'Report escapes candidate directory'
                report = read_json(report_path)
                assert digest(report_path.read_bytes()) == approved[job['id']]['report_sha256']
                assert report['source_sha256'] == digest((out / 'source.json').read_bytes()), 'Stale source report'
                assert report['job'] == job['id'], 'Wrong sequence report'
                assert sorted((f['direction'], f['frame']) for f in report['frames']) == sorted(tuple(c) for c in job['cells']), 'Missing, duplicate or wrong sequence frames'
                assert approved[job['id']]['visual_review'] == 'pass'
                for f in report['frames']:
                    image_path = out / f['hd']
                    assert image_path.resolve().is_relative_to(out.resolve()), 'Frame escapes candidate directory'
                    raw = image_path.read_bytes(); assert digest(raw) == f['sha256']
                    original_info = action['sequences'][f['direction']][f['frame']]
                    assert f['original'] == original_info['file'], 'Wrong pose source'
                    replacement = Image.open(io.BytesIO(raw))
                    assert replacement.mode == 'RGBA', 'Replacement must preserve real RGBA alpha'
                    assert max(replacement.size) <= 1024, 'Sprite exceeds texture loader bound'
                    assert replacement.size == tuple(v * 4 for v in original_info['frame_size']), 'Wrong 4x canvas'
                    alpha = np.asarray(replacement)[:, :, 3]
                    assert set(np.unique(alpha)) == {0, 255}, 'Replacement alpha must be binary with visible and transparent pixels'
                    assert not any(np.any(edge) for edge in (alpha[0], alpha[-1], alpha[:, 0], alpha[:, -1])), 'Clipped replacement sprite'
                    frame = entry['fp'][f['direction']][f['frame']]
                    name = f'restoration-fp/{action["action"]}/d{f["direction"]}-{f["frame"]:02}.png'
                    assert name not in z.namelist()
                    frame['file'] = name
                    for field in ('frame_w', 'frame_h', 'offset_x', 'offset_y', 'shadow_offset', 'offset_w', 'offset_h'):
                        frame[field] *= 4
                    replacements[name] = raw
            entry['fp_resolution_scale'] = 4
            completed.append(action['action'])
        assert completed, 'No complete visually reviewed FP action'
        target.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as dest:
            for item in z.infolist():
                if item.filename != 'sprites.json': dest.writestr(copy.copy(item), z.read(item.filename))
            dest.writestr(copy.copy(z.getinfo('sprites.json')), json.dumps(entries, separators=(',', ':')))
            for name, raw in replacements.items():
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                dest.writestr(info, raw)
        with zipfile.ZipFile(target) as candidate:
            assert candidate.testzip() is None
            for name in z.namelist():
                if name != 'sprites.json': assert candidate.read(name) == z.read(name), 'Existing artwork changed'
            for before, after in zip(original_entries, entries):
                assert before['td'] == after['td']
                assert before['name'] == after['name'] and before['rotatable'] == after['rotatable']
                assert len(before['fp']) == len(after['fp'])
                assert all(len(a) == len(b) for a, b in zip(before['fp'], after['fp']))
    report = {'schema': 1, 'status': 'candidate_static_review_only_runtime_pending', 'local_only': True,
        'baseline_manifest': source['baseline_manifest'], 'baseline_archive_sha256': source['model']['archive_sha256'],
        'archive': str(target), 'archive_sha256': digest(target.read_bytes()), 'completed_fp_actions': completed,
        'new_frames': len(replacements), 'existing_artwork_members_byte_identical': True, 'td_metadata_identical': True,
        'gameplay_config_unchanged': True, 'runtime_installed': False}
    save_json(target.with_suffix('.report.json'), report); print(json.dumps(report))

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('command', choices=('inventory', 'prepare', 'process', 'package'))
    ap.add_argument('--workspace', type=Path, required=True,
                    help='Workspace containing runtime/keeperfx-x64; never inferred from the checkout location')
    ap.add_argument('--manifest', type=Path, help='Local combined review manifest used for inventory')
    ap.add_argument('--version', default='v1')
    ap.add_argument('--output', type=Path, required=True, help='Local preparation directory, outside the source checkout')
    ap.add_argument('--slug'); ap.add_argument('--job'); ap.add_argument('--target', type=Path)
    args = ap.parse_args()
    if not __debug__: ap.error('Run without -O: source preservation assertions are required')
    if args.command != 'inventory' and not (args.slug and re.fullmatch(r'[a-z0-9][a-z0-9_]*', args.slug)):
        ap.error('A valid --slug is required for this command')
    if args.command == 'process' and not args.job: ap.error('process requires --job')
    if args.command == 'package' and not args.target: ap.error('package requires --target')
    output = args.output.resolve()
    if args.command == 'inventory': inventory(args.workspace.resolve(), output, args.manifest)
    elif args.command == 'prepare': prepare(args.workspace, output, args.slug)
    elif args.command == 'process': process(args.workspace, output, args.slug, args.job, args.version)
    else: package(args.workspace, output, args.slug, args.target)

if __name__ == '__main__': main()
