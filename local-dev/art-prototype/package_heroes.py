# SPDX-License-Identifier: GPL-2.0-or-later
"""Compose complete local hero packages and the accepted imp without altering art."""
from pathlib import Path
import hashlib,json,shutil,zipfile
ROOT=Path(__file__).resolve().parents[3]
ART=ROOT/'artifacts/art-prototype'
OUT=ART/'heroes';DEST=OUT/'mod/heroes_art_prototype'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    roster=json.loads((OUT/'inventory.json').read_text());sources=[];names=set();frames=0;held=0
    for entry in roster:
        slug=entry['slug'];folder=OUT/slug;report=json.loads((folder/'package-report.json').read_text())
        z=folder/f'mod/hero_{slug}_art_prototype/fxdata/{slug}-sprites.zip'
        assert sha(z)==report['zip_sha256'],slug
        assert report['overhead_frames']==entry['td_frames'] and report['enhanced_hand_frames']==6,slug
        with zipfile.ZipFile(z) as archive:
            assert archive.testzip() is None
            animations=json.loads(archive.read('sprites.json'))
            for a in animations:assert a['name'] not in names;names.add(a['name'])
        sources.append((slug,z,folder/f'mod/hero_{slug}_art_prototype/creatrs/{slug}.cfg',report))
        frames+=report['overhead_frames'];held+=6
    imp=ART/'imp-actions/mod/imp_art_prototype';impz=imp/'fxdata/imp-sprites.zip'
    imp_expected='a1c742fcd4d207de741b0432723910633da3e35db60afb97477dc242fdf89819'
    assert sha(impz)==imp_expected,'Accepted imp package changed'
    with zipfile.ZipFile(impz) as archive:
        assert archive.testzip() is None
        for a in json.loads(archive.read('sprites.json')):assert a['name'] not in names;names.add(a['name'])
    if DEST.exists():assert (DEST/'local-prototype.json').exists(),'Refuse unmarked existing combined mod'
    (DEST/'fxdata').mkdir(parents=True,exist_ok=True);(DEST/'creatrs').mkdir(exist_ok=True)
    records=[]
    for slug,z,cfg,report in sources+ [('imp',impz,imp/'creatrs/imp.cfg',{'zip_sha256':imp_expected})]:
        assert cfg.read_text().startswith('[sprites]'),slug
        shutil.copy2(z,DEST/'fxdata'/z.name);shutil.copy2(cfg,DEST/'creatrs'/cfg.name)
        records.append(dict(slug=slug,zip=z.name,zip_sha256=sha(z),config=cfg.name,config_sha256=sha(cfg)))
    manifest_sha=hashlib.sha256(json.dumps(records,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    report=dict(status='assembled_pending_combined_game_and_performance_review',heroes=len(roster),hero_overhead_frames=frames,hero_held_frames=held,accepted_imp_overhead_frames=197,accepted_imp_held_frames=6,total_overhead_frames=frames+197,total_held_frames=held+6,animation_entries=len(names),package_manifest_sha256=manifest_sha,packages=records,scope='Original13hero unique TD and FPPowerGrab at4x plus acceptedimp; otherpossession/shared effects original',user_visual_acceptance='Imp accepted; hero review pending',local_only=True)
    (OUT/'combined-package-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (DEST/'local-prototype.json').write_text(json.dumps(dict(local_only=True,kind='heroes_and_accepted_imp',package_manifest_sha256=manifest_sha,packages=records,commercial_source_assets='Local only; do not publish derived art or runtime packages'),indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='packages'},indent=2))
if __name__=='__main__':main()
