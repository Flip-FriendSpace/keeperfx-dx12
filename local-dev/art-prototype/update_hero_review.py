# SPDX-License-Identifier: GPL-2.0-or-later
"""Refresh the local roster review; report evidence without inferring acceptance."""
from pathlib import Path
import json
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'artifacts/art-prototype/heroes'
TEMPLATE=Path(__file__).with_name('hero_review.html').read_text(encoding='utf-8')
rows=[]
for item in json.loads((OUT/'inventory.json').read_text()):
    slug=item['slug']; folder=OUT/slug
    package=json.loads((folder/'package-report.json').read_text()) if (folder/'package-report.json').exists() else None
    sources=json.loads((folder/'source-status.json').read_text()) if (folder/'source-status.json').exists() else {}
    tests=[]
    if package:
        for p in sorted(folder.glob('HeroValidation-HD-*/run-report.json')):
            r=json.loads(p.read_text(encoding='utf-8-sig'))
            if r.get('passed') and r.get('settingsRestored') and r.get('packageSHA256','').lower()==package['zip_sha256']:
                tests.append(p.parent.name)
    latest=tests[-1] if tests else None
    if package:
        html=TEMPLATE.replace('__HERO__',slug).replace('__FRAMES__',str(item['td_frames']))
        if latest:
            html=html.replace('Game evidence is listed in the roster review; user visual acceptance remains pending.',f'<a href="{latest}/fixture-report.json">Game action test passed for this package.</a> User visual review is recorded in prototype.json; ordinary campaign play remains pending.')
        html=html.replace('user visual acceptance remains pending.', 'user visual review is recorded in prototype.json; ordinary campaign play remains pending.')
        (folder/'review.html').write_text(html,encoding='utf-8')
    rows.append(dict(slug=slug,overhead=item['td_frames'],packed=bool(package),test=latest,source_status=sources.get('status','Artwork in progress'),thumbnail=f'{slug}/frames/idle-td-d1-01-hd.png' if package else None))
(OUT/'roster-status.json').write_text(json.dumps(rows,indent=2)+'\n',encoding='utf-8')
html='''<!doctype html><html lang="en"><meta charset="utf-8"><title>KeeperFX hero review</title>
<style>body{margin:0;background:#121b17;color:#ede9dc;font:16px system-ui}header{padding:26px 30px;border-bottom:1px solid #3b493c}h1{margin:0 0 12px;font-size:26px}p{line-height:1.5;max-width:1100px}#roster{display:flex;gap:10px;overflow:auto;padding:20px 30px}button{background:#24352b;border:1px solid #55634e;color:inherit;border-radius:8px;padding:12px;min-width:160px;text-align:left;cursor:pointer}button[disabled]{opacity:.55;cursor:default}button.active{border-color:#e0c17c}button img{display:block;height:100px;width:140px;object-fit:contain;image-rendering:pixelated}small{display:block;line-height:1.5;color:#c6c8b8}iframe{display:block;width:100%;height:780px;border:0}a{color:#e0c17c}#note{margin:0 30px}</style>
<header><h1>Dungeon Keeper heroes · 4× sprite review</h1><p>Compare the enhanced heroes with their original actions at the same displayed size. Choose a hero, action and facing; pause or step through individual frames. Packed artwork and completed game tests are listed separately. User visual review is recorded in prototype.json; ordinary campaign play remains pending.</p><p><a href="../imp-actions/review.html">Accepted imp</a> · <a href="roster-status.json">Roster evidence</a> · <a href="integration-report.json">Combined validation and limits</a></p></header><nav id="roster" aria-label="Heroes"></nav><p id="note"></p><iframe title="Selected hero animation comparison"></iframe><script>
const rows=__DATA__;const nav=document.getElementById('roster'),note=document.getElementById('note'),viewer=document.querySelector('iframe');
for(const r of rows){const b=document.createElement('button');b.disabled=!r.packed;b.innerHTML=(r.thumbnail?`<img src="${r.thumbnail}" alt="${r.slug}">`:'')+`<strong>${r.slug}</strong><small>${r.overhead} overhead + 6 held frames</small><small>${r.test?'Game test passed':r.packed?'Packed · game review pending':'Artwork in progress'}</small>`;b.onclick=()=>{document.querySelectorAll('button').forEach(x=>x.classList.remove('active'));b.classList.add('active');viewer.src=r.slug+'/review.html';note.innerHTML=r.test?`Game evidence: <a href="${r.slug}/${r.test}/fixture-report.json">actions and transitions</a>. Visual review recorded in prototype.json; ordinary campaign play pending.`:'Game validation pending.';};nav.append(b);}nav.querySelector('button:not([disabled])')?.click();
</script></html>'''.replace('__DATA__',json.dumps(rows))
(OUT/'review.html').write_text(html,encoding='utf-8')
print(f'{sum(r["packed"] for r in rows)}/{len(rows)} heroes packed; {sum(bool(r["test"]) for r in rows)} game fixtures passed.')
