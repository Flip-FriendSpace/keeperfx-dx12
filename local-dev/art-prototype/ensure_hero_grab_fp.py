# SPDX-License-Identifier: GPL-2.0-or-later
"""Add a missing explicit held-view job to the selected hero only."""
from pathlib import Path
import argparse,json
ap=argparse.ArgumentParser();ap.add_argument('slug');args=ap.parse_args()
root=Path(__file__).resolve().parents[3]/'artifacts/art-prototype/heroes'/args.slug
path=root/'generation-plan.json';plan=json.loads(path.read_text());meta=json.loads((root/'source-manifest.json').read_text())
for d,frames in enumerate(meta['grab']['fp']):
    if any(j['action']=='grab' and j['view']=='fp' and j['direction']==d for j in plan['jobs']):continue
    count=len(frames);cols=3 if count==6 else min(count,4);rows=(count+cols-1)//cols
    plan['jobs'].append({'action':'grab','view':'fp','direction':d,'count':count,'cols':cols,'rows':rows,'reference':f'references/grab-fp-d{d}-reference.png','output':f'grab-fp-d{d}-sheet.png','status':'pending_generation'})
path.write_text(json.dumps(plan,indent=2)+'\n')
