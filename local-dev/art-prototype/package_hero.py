# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack one fully generated hero at faithful 4x, without runtime deployment."""
from pathlib import Path
import argparse
import ast
import hashlib
import io
import json
import zipfile
import numpy as np
from PIL import Image, ImageFilter

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent

def primitives(out):
    # Reuse accepted implementations without package_imp_set import mkdir/read effects.
    tree=ast.parse((HERE/'package_imp_set.py').read_text())
    nodes=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in ('split','bounds','register_sequence','engine_png')]
    pal=np.frombuffer((ROOT/'runtime/keeperfx-x64/data/png_conv_pal.dat').read_bytes(),dtype=np.uint8).reshape(256,3)
    env={'np':np,'Image':Image,'ImageFilter':ImageFilter,'OUT':out,'palette':pal,'palette_rgb':(pal.astype(np.uint16)*4).astype(np.uint8)}
    exec(compile(ast.Module(body=nodes,type_ignores=[]),str(HERE/'package_imp_set.py'),'exec'),env)
    return env

def main():
    ap=argparse.ArgumentParser();ap.add_argument('slug');ap.add_argument('--check-only',action='store_true')
    ap.add_argument('--max-dimension',type=int,choices=(254,512,1024),default=254,
                    help='Runtime source limit; values above254 require the separately validated large-sprite engine.')
    args=ap.parse_args()
    out=ROOT/'artifacts/art-prototype/heroes'/args.slug
    meta=json.loads((out/'source-manifest.json').read_text());plan=json.loads((out/'generation-plan.json').read_text())
    blockers=[]
    for action,m in meta.items():
        for view in (('td','fp') if action=='grab' else ('td',)):
            for d,seq in enumerate(m[view]):
                for n,info in enumerate(seq):
                    if max(info['frame_size'])*4>args.max_dimension:blockers.append({'action':action,'view':view,'direction':d,'frame':n,'required_4x':[v*4 for v in info['frame_size']]})
    missing=[j['output'] for j in plan['jobs'] if not (out/j['output']).exists()]
    summary={'slug':args.slug,'missing_sheets':missing,'dimension_blockers':blockers,'status':'blocked' if blockers or missing else 'ready_to_pack'}
    print(json.dumps(summary,indent=2))
    if args.check_only:return
    if blockers or missing:raise SystemExit('Cannot pack: exact 4x dimensions or generated sheets unavailable; no fallback upscale or clipping permitted.')
    p=primitives(out);frames=out/'frames';frames.mkdir(exist_ok=True);review={};checks=[];hashes=[]
    jobs={(j['action'],j['view'],j['direction']):j for j in plan['jobs']}
    sequences=[(action,view,m) for action,m in meta.items() for view in (('td','fp') if action=='grab' else ('td',))]
    for action,view,m in sequences:
        review_key=action if view=='td' else 'grab-hand'
        review[review_key]=[]
        for d,infos in enumerate(m[view]):
            job=jobs[action,view,d];path=out/job['output'];source=Image.open(path).convert('RGBA')
            assert source.getchannel('A').getextrema()[0]==0, f'{path}: no transparent pixels'
            selected=job.get('cell_indices',list(range(job['count'])))
            cells=p['split'](path,job['cols'],job['rows'],required_cells=selected)
            cells=[cells[i] for i in selected]
            assert len(cells)==job['count']==len(infos)
            images=p['register_sequence'](cells,infos,out/'references');seq=[]
            hashes.append({'file':job['output'],'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
            for n,(im,info) in enumerate(zip(images,infos)):
                basename=f'{action}-{view}-d{d}-{n+1:02}';b=p['bounds'](im)
                assert b and b[0]>0 and b[1]>0 and b[2]<im.width and b[3]<im.height, (basename,b)
                assert max(im.size)<=args.max_dimension
                variants={'rgba':im,'hd':p['engine_png'](im),'native':p['engine_png'](im.resize(tuple(info['frame_size']),Image.Resampling.LANCZOS))}
                for tag,img in variants.items():img.save(frames/f'{basename}-{tag}.png')
                seq.append({'original':f'references/{info["file"]}',**{tag:f'frames/{basename}-{tag}.png' for tag in variants},'size':info['frame_size'],'anchor':[-v for v in info['anchor']]})
                checks.append({'action':action,'view':view,'direction':d,'frame':n,'size':list(im.size),'bounds':b})
            review[review_key].append(seq)
    mod=out/f'mod/hero_{args.slug}_art_prototype';(mod/'fxdata').mkdir(parents=True,exist_ok=True);(mod/'creatrs').mkdir(exist_ok=True)
    zip_path=mod/'fxdata'/f'{args.slug}-sprites.zip';entries=[]
    with zipfile.ZipFile(zip_path,'w',zipfile.ZIP_DEFLATED) as archive:
        for action,m in meta.items():
            entry={'name':f'ARTHERO_{args.slug.upper()}_HD_{action.upper()}','rotatable':m['rotatable'],'td_resolution_scale':4,'fp_resolution_scale':4 if action=='grab' else 1,'td':[],'fp':[]}
            for view in ('td','fp'):
                for d,infos in enumerate(m[view]):
                    seq=[];factor=entry[f'{view}_resolution_scale']
                    for n,info in enumerate(infos):
                        path=frames/f'{action}-{view}-d{d}-{n+1:02}-hd.png' if factor==4 else out/'references'/info['file']
                        im=Image.open(path).convert('RGBA');assert max(im.size)<=args.max_dimension
                        name=f'{action}/{view}/d{d}-{n+1:02}.png';buf=io.BytesIO();im.save(buf,format='PNG');archive.writestr(name,buf.getvalue())
                        seq.append({'file':name,'frame_w':im.width,'frame_h':im.height,'offset_x':-info['anchor'][0]*factor,'offset_y':-info['anchor'][1]*factor,'shadow_offset':0,'offset_w':0,'offset_h':0})
                    entry[view].append(seq)
            assert len(entry['td'])==len(entry['fp'])==(5 if entry['rotatable'] else 1)
            entries.append(entry)
        archive.writestr('sprites.json',json.dumps(entries,separators=(',',':')))
    with zipfile.ZipFile(zip_path) as archive:assert archive.testzip() is None
    (mod/'creatrs'/f'{args.slug}.cfg').write_text('[sprites]\n'+''.join(f'{alias} = ARTHERO_{args.slug.upper()}_HD_{action.upper()}\n' for alias,action in plan['aliases'].items()))
    td_count=sum(c['view']=='td' for c in checks);fp_count=sum(c['view']=='fp' for c in checks)
    report={'status':'packed_pending_visual_and_game_review','overhead_frames':td_count,'enhanced_hand_frames':fp_count,'fp_original_fallback':'All except PowerGrab','powergrab_fp':'enhanced_4x','required_max_dimension':max(max(c['size']) for c in checks),'configured_max_dimension':args.max_dimension,'shared_original':plan['shared_original'],'zip_sha256':hashlib.sha256(zip_path.read_bytes()).hexdigest(),'sources':hashes,'frames':checks,'local_only':True}
    (mod/'local-prototype.json').write_text(json.dumps({'local_only':True,'hero':args.slug,'commercial_source_assets':'Do not publish this local package','package_sha256':report['zip_sha256']},indent=2)+'\n')
    (out/'package-report.json').write_text(json.dumps(report,indent=2)+'\n');(out/'review-data.json').write_text(json.dumps(review));(out/'review-data.js').write_text('window.impReviewData='+json.dumps(review)+';\n')
    html=(HERE/'hero_review.html').read_text(encoding='utf-8').replace('__HERO__',args.slug).replace('__FRAMES__',str(td_count))
    (out/'review.html').write_text(html,encoding='utf-8')

if __name__=='__main__':main()
