# SPDX-License-Identifier: GPL-2.0-or-later
"""Save a generated local hero sheet and merge its explicit plan job fields."""
from pathlib import Path
import argparse,json,shutil,hashlib
from PIL import Image
ap=argparse.ArgumentParser();ap.add_argument('slug');ap.add_argument('action');ap.add_argument('view');ap.add_argument('direction');ap.add_argument('source');ap.add_argument('output');ap.add_argument('prompt');ap.add_argument('--cols',type=int);ap.add_argument('--rows',type=int);args=ap.parse_args()
ROOT=Path(__file__).resolve().parents[3];out=ROOT/'artifacts/art-prototype/heroes'/args.slug
path=out/args.output
if path.exists():raise SystemExit('Refusing to overwrite an existing art candidate')
shutil.copyfile(args.source,path);im=Image.open(path).convert('RGBA');assert im.getchannel('A').getextrema()[0]==0
plan_path=out/'generation-plan.json';plan=json.loads(plan_path.read_text())
for job in plan['jobs']:
    if job['action']!=args.action or job['view']!=args.view:continue
    if args.direction!='all' and job['direction']!=int(args.direction):continue
    job.update(output=args.output,prompt=args.prompt,status='generated_pending_sequence_and_game_review',sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    if args.cols:job['cols']=args.cols
    if args.rows:job['rows']=args.rows
    if args.direction=='all':
        assert job['count']==1
        job['cell_indices']=[job['direction']]
plan_path.write_text(json.dumps(plan,indent=2)+'\n')
print(json.dumps({'file':str(path),'size':im.size,'alpha':im.getchannel('A').getextrema(),'jobs_updated':args.action}))
