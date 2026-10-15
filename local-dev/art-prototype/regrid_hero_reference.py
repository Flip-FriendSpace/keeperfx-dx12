# SPDX-License-Identifier: GPL-2.0-or-later
"""Build an explicit alternative equal-cell reference grid from original frames."""
from pathlib import Path
import argparse,json,math
from PIL import Image
ap=argparse.ArgumentParser();ap.add_argument('slug');ap.add_argument('action');ap.add_argument('cols',type=int);args=ap.parse_args()
root=Path(__file__).resolve().parents[3]/'artifacts/art-prototype/heroes'/args.slug
meta=json.loads((root/'source-manifest.json').read_text())[args.action]
plan_path=root/'generation-plan.json';plan=json.loads(plan_path.read_text())
for d,infos in enumerate(meta['td']):
    rows=math.ceil(len(infos)/args.cols);images=[Image.open(root/'references'/i['file']).convert('RGBA') for i in infos]
    w=max(im.width for im in images);h=max(im.height for im in images);factor=min(12,1800//max(w*args.cols,h*rows))
    sheet=Image.new('RGBA',(w*args.cols*factor,h*rows*factor))
    for n,im in enumerate(images):sheet.alpha_composite(im.resize((im.width*factor,im.height*factor),Image.Resampling.NEAREST),((n%args.cols)*w*factor,(n//args.cols)*h*factor))
    name=f'references/{args.action}-td-d{d}-reference-{args.cols}x{rows}.png';sheet.save(root/name)
    for job in plan['jobs']:
        if job['action']==args.action and job['view']=='td' and job['direction']==d:job.update(reference=name,cols=args.cols,rows=rows)
plan_path.write_text(json.dumps(plan,indent=2)+'\n')
