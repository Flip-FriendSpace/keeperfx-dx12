# SPDX-License-Identifier: GPL-2.0-or-later
"""Inspect generated job slicing/registration without publishing an incomplete ZIP."""
from pathlib import Path
import argparse,json
from PIL import Image
from package_hero import primitives
ap=argparse.ArgumentParser();ap.add_argument('slug');args=ap.parse_args()
root=Path(__file__).resolve().parents[3]/'artifacts/art-prototype/heroes'/args.slug
meta=json.loads((root/'source-manifest.json').read_text());plan=json.loads((root/'generation-plan.json').read_text());p=primitives(root);checks=[]
dest=root/'generated-audit';dest.mkdir(exist_ok=True)
for job in plan['jobs']:
    path=root/job['output']
    if not path.exists():continue
    action,view,d=job['action'],job['view'],job['direction'];infos=meta[action][view][d]
    cells=job.get('cell_indices',list(range(job['count'])))
    split=p['split'](path,job['cols'],job['rows'],required_cells=cells);images=[split[i] for i in cells]
    assert len(images)==len(infos)
    registered=p['register_sequence'](images,infos,root/'references')
    for n,im in enumerate(registered):
        b=p['bounds'](im);assert b and b[0]>0 and b[1]>0 and b[2]<im.width and b[3]<im.height,(action,view,d,n,b)
    loop=[im.resize((im.width*2,im.height*2),Image.Resampling.NEAREST) for im in registered]
    loop[0].save(dest/f'{action}-{view}-d{d}.gif',save_all=True,append_images=loop[1:],duration=125,loop=0,disposal=2)
    checks.append({'action':action,'view':view,'direction':d,'count':len(images),'sizes':[im.size for im in registered]})
(dest/'report.json').write_text(json.dumps({'status':'generated_sequences_audited_not_packaged','jobs':checks},indent=2)+'\n')
print(json.dumps({'slug':args.slug,'audited_jobs':len(checks),'frames':sum(j['count'] for j in checks)}))
