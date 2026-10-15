# SPDX-License-Identifier: GPL-2.0-or-later
"""Extend the accepted local imp ZIP with the remaining original action sets.

Mechanical slicing/registration only; generated artwork remains outside Git.
The accepted idle/walk/dig entries and PNG bytes are copied without conversion.
"""
from pathlib import Path
import hashlib
import io
import json
import zipfile
from PIL import Image
from package_imp_set import split, register_sequence, engine_png, bounds

ROOT = Path(__file__).resolve().parents[3]
ART = ROOT / 'artifacts/art-prototype'
OUT = ART / 'imp-actions'
BASE = ART / 'imp-set'
META = json.loads((OUT / 'source-manifest.json').read_text())
FRAMES = OUT / 'frames'
MOD = OUT / 'mod/imp_art_prototype'
FRAMES.mkdir(exist_ok=True)
(MOD / 'fxdata').mkdir(parents=True, exist_ok=True)
(MOD / 'creatrs').mkdir(exist_ok=True)
ALIASES = {
    'Stand':'idle', 'Ambulate':'walk', 'Drag':'drag',
    'Attack':'dig', 'RangedAttack':'dig', 'CastSpell':'dig', 'Dig':'dig',
    'Smoke':'smoke', 'Relax':'relax', 'PrettyDance':'claim', 'GotHit':'hit',
    'PowerGrab':'grab', 'GotSlapped':'slap', 'Celebrate':'claim',
    'Sleep':'idle', 'EatChicken':'idle', 'Scream':'scream', 'DropDead':'death',
    'Roar':'smoke', 'Piss':'relax', 'Custom':'scream',
}

review = json.loads((BASE / 'review-data.json').read_text())
for action in review.values():
    for direction in action:
        for frame in direction:
            for key in ['original', 'rgba', 'native', 'hd']:
                frame[key] = '../imp-set/' + frame[key]
checks = []
source_files = []
def source_path(name):
    path = OUT / name
    source_files.append({'file':name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    return path

for action, meta in META.items():
    if action in ('relax', 'drag', 'smoke'):
        cols, rows = (2, 1) if action == 'relax' else (4 if action == 'smoke' else 3, 2)
        sources = [split(source_path(f'{action}-d{d}-v{3 if d == 4 else 2}-sheet.png'), cols, rows) for d in range(5)]
    elif action in ('hit', 'slap'):
        count = len(meta['td'][0])
        cells = split(source_path(f'{action}-sheet.png'), count, 5)
        sources = [cells[d*count:(d+1)*count] for d in range(5)]
    elif action in ('scream', 'death', 'grab'):
        cols, rows = {'scream':(4,1), 'death':(2,1), 'grab':(3,2)}[action]
        sources = [split(source_path(f'{action}-sheet.png'), cols, rows)]
    else:
        cols = 4 if action == 'smoke' else 3
        sources = [split(source_path(f'{action}-d{d}-sheet.png'), cols, 2) for d in range(5)]
    views = [('td', sources)]
    if action == 'grab':
        views.append(('fp', [split(source_path('grab-fp-alpha.png'), 3, 2)]))
    for view, directions in views:
        review_key = action if view == 'td' else 'grab-hand'
        review[review_key] = []
        for d, images in enumerate(directions):
            infos = meta[view][d]
            assert len(images) == len(infos)
            registered = register_sequence(images, infos, OUT / 'references')
            seq = []
            for n, (rgba, info) in enumerate(zip(registered, infos)):
                basename = f'{action}-{view}-d{d}-{n+1:02}'
                native = rgba.resize(tuple(info['frame_size']), Image.Resampling.LANCZOS)
                converted = {'rgba':rgba, 'native':engine_png(native), 'hd':engine_png(rgba)}
                for tag, im in converted.items():
                    im.save(FRAMES / f'{basename}-{tag}.png')
                hd = converted['hd']; b = bounds(hd)
                assert b and b[0] > 0 and b[1] > 0 and b[2] < hd.width and b[3] < hd.height, (basename, b)
                assert max(hd.size) < 255
                checks.append({'action':action,'view':view,'direction':d,'frame':n+1,'size':hd.size,'bounds':b})
                seq.append({'original':f'references/{info["file"]}',
                            **{tag:f'frames/{basename}-{tag}.png' for tag in converted},
                            'size':info['frame_size'], 'anchor':[-v for v in info['anchor']]})
            review[review_key].append(seq)

base_zip = BASE / 'mod/imp_art_prototype/fxdata/imp-sprites.zip'
zip_path = MOD / 'fxdata/imp-sprites.zip'
with zipfile.ZipFile(base_zip) as old, zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as archive:
    entries = json.loads(old.read('sprites.json'))
    for name in old.namelist():
        if name != 'sprites.json':
            archive.writestr(name, old.read(name))
    for action, meta in META.items():
        entry = {'name':f'ARTIMP_HD_{action.upper()}', 'rotatable':len(meta['td']) == 5,
                 'td_resolution_scale':4, 'fp_resolution_scale':4 if action == 'grab' else 1,
                 'td':[], 'fp':[]}
        for view in ('td', 'fp'):
            for d, infos in enumerate(meta[view]):
                seq = []
                for n, info in enumerate(infos):
                    factor = entry[f'{view}_resolution_scale']
                    path = FRAMES / f'{action}-{view}-d{d}-{n+1:02}-hd.png' if factor == 4 else OUT / 'references' / info['file']
                    im = Image.open(path).convert('RGBA')
                    assert max(im.size) < 255
                    name = f'hd/{action}/{view}/d{d}-{n+1:02}.png'
                    buffer = io.BytesIO(); im.save(buffer, format='PNG'); archive.writestr(name, buffer.getvalue())
                    seq.append({'file':name,'frame_w':im.width,'frame_h':im.height,
                                'offset_x':-info['anchor'][0]*factor,'offset_y':-info['anchor'][1]*factor,
                                'shadow_offset':0,'offset_w':0,'offset_h':0})
                entry[view].append(seq)
        entries.append(entry)
    archive.writestr('sprites.json', json.dumps(entries, separators=(',', ':')))
with zipfile.ZipFile(base_zip) as old, zipfile.ZipFile(zip_path) as archive:
    assert archive.testzip() is None
    assert all(archive.read(n) == old.read(n) for n in old.namelist() if n != 'sprites.json')
    assert entries[:6] == json.loads(old.read('sprites.json'))
    assert len(entries) == 15
    for entry in entries:
        assert len(entry['td']) == len(entry['fp']) == (5 if entry['rotatable'] else 1)
        assert len({len(s) for s in entry['td'] + entry['fp']}) == 1
    png_count = sum(n.endswith('.png') for n in archive.namelist())

(MOD / 'creatrs/imp.cfg').write_text('[sprites]\n' + ''.join(f'{key} = ARTIMP_HD_{action.upper()}\n' for key, action in ALIASES.items()))
(MOD / 'local-prototype.json').write_text(json.dumps({'local_only':True,
    'overhead_states':list(review)[:-1] if list(review)[-1] == 'grab-hand' else [k for k in review if k != 'grab-hand'],
    'hand':'Six enhanced PowerGrab first-person frames',
    'possession':'All other first-person frames retained at original resolution',
    'shared_original':['Torture','DeadSplat','QuerySymbol','HandSymbol'],
    'commercial_source_assets':'Do not publish this local package'}, indent=2)+'\n')
(OUT / 'review-data.json').write_text(json.dumps(review, separators=(',', ':')))
(OUT / 'review-data.js').write_text('window.impReviewData='+json.dumps(review, separators=(',', ':'))+';\n')
report = {'overhead_frames':197,'additional_overhead_frames':132,'enhanced_hand_frames':6,
          'sprite_entries':len(entries),'runtime_pngs':png_count,'original_65_preserved':True,
          'zip_sha256':hashlib.sha256(zip_path.read_bytes()).hexdigest(),
          'base_zip_sha256':hashlib.sha256(base_zip.read_bytes()).hexdigest(),
          'aliases':ALIASES,'frames':checks,'touching_edges':[], 'sources':source_files,
          'status':'packed_pending_visual_and_game_review'}
assert len(checks) == 138
(OUT / 'package-report.json').write_text(json.dumps(report, indent=2)+'\n')
(OUT / 'review.html').write_bytes(Path(__file__).with_name('imp_actions_review.html').read_bytes())
print(json.dumps({k:v for k,v in report.items() if k not in ('frames','aliases','sources')}))
