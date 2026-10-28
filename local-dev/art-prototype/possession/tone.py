# SPDX-License-Identifier: GPL-2.0-or-later
"""Source-bound palette tone curves for possession candidates, without geometry edits."""
from pathlib import Path
import argparse, json, re
import numpy as np
from PIL import Image, ImageDraw
import restore, colour, colour_math as cm

Q = [0,.1,.5,.9,.95,.99,1]

def stats(indices, pal):
    rgb=pal.pal[indices];lab=cm.lab(cm.to_linear(rgb))
    return {'L_quantiles':np.quantile(lab[:,0],Q).tolist(),
        'RGB_quantiles':np.quantile(rgb,[.5,.9,.95,.99],axis=0).tolist(),
        'mean_L':float(lab[:,0].mean()),'histogram':np.bincount(indices,minlength=256).tolist()}

def tone_lut(source, candidate, pal):
    """One monotone luminance quantile curve, quantized to source-used colours."""
    lab=cm.lab(cm.to_linear(pal.pal));src_L=lab[source,0];dst_L=lab[candidate,0]
    sx,sc=np.unique(src_L,return_counts=True);dx,dc=np.unique(dst_L,return_counts=True)
    sp=(np.cumsum(sc)-sc/2)/sc.sum();dp=(np.cumsum(dc)-dc/2)/dc.sum()
    targets=np.interp(dp,sp,sx);wanted=np.interp(lab[:,0],dx,targets)
    desired=lab.copy();desired[:,0]=wanted
    allowed=np.unique(source)
    delta=desired[:,None,:]-lab[allowed][None,:,:]
    delta[:,:,0]*=8
    lut=allowed[np.square(delta).sum(-1).argmin(1)]
    return lut, {'candidate_L_knots':dx.tolist(),'source_L_targets':targets.tolist(),
        'source_allowed_indices':allowed.tolist(),'palette_lut':lut.tolist(),
        'method':'One fixed monotone L* quantile curve pooled over every original/candidate direction and frame of the action; nearest source-used palette colour after 8x L* distance weighting. No per-frame fit or pixel-position edits.'}

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--slug',required=True);ap.add_argument('--tag',required=True)
    ap.add_argument('--workspace',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True,help='Preparation directory passed to restore.py')
    args=ap.parse_args()
    if not __debug__:ap.error('Run without -O: source preservation assertions are required')
    if not re.fullmatch(r'[a-z0-9][a-z0-9_]*',args.slug):ap.error('Invalid creature slug')
    if args.slug=='ghost':raise ValueError('Ghost requires dedicated additive processing')
    assert re.fullmatch(r'[a-z0-9][a-z0-9-]*',args.tag)
    out=args.output.resolve()/args.slug;target=out/'tone'/args.tag
    assert not target.exists(), 'Refusing existing tone candidate'
    source=restore.read_json(out/'source.json');plan=restore.read_json(out/'generation-plan.json')
    restore.authenticate_source(out,source);selected=restore.read_json(out/'selection.json')['jobs']
    pal=colour.Palette(args.workspace,vars(cm));receipt={'schema':1,'tag':args.tag,'actions':[],'visual_review':'pending','runtime':'not_run'}
    target.mkdir(parents=True)
    for action in source['actions']:
        jobs=restore.action_jobs(plan,action)
        if not all(j['id'] in selected for j in jobs):continue
        inputs=[];original=[];candidate=[]
        for job in jobs:
            rp=out/selected[job['id']]['report'];report=restore.read_json(rp)
            assert restore.digest(rp.read_bytes())==selected[job['id']]['report_sha256']
            assert report['source_sha256']==restore.digest((out/'source.json').read_bytes())
            assert sorted((f['direction'],f['frame']) for f in report['frames'])==sorted(tuple(c) for c in job['cells'])
            for f in report['frames']:
                assert f['original']==action['sequences'][f['direction']][f['frame']]['file']
                raw=(out/f['hd']).read_bytes();assert restore.digest(raw)==f['sha256']
                for store,key in [(original,'original'),(candidate,'hd')]:
                    a=np.asarray(Image.open(out/f[key]).convert('RGBA'));idx=pal.index(a[:,:,:3]);store.append(idx[a[:,:,3]>=64])
            inputs.append((job,report,rp))
        src=np.concatenate(original);before=np.concatenate(candidate);lut,curve=tone_lut(src,before,pal)
        expected_after=lut[before];records=[]
        for job,report,rp in inputs:
            newframes=[];panels=[]
            for f in report['frames']:
                a=np.asarray(Image.open(out/f['hd']).convert('RGBA'));idx=pal.index(a[:,:,:3]);mapped=lut[idx]
                corrected=a.copy();corrected[:,:,:3]=(pal.conv.astype(np.uint16)*4)[mapped]
                assert np.array_equal(corrected[:,:,3],a[:,:,3]), 'Alpha changed'
                assert np.array_equal(pal.index(corrected[:,:,:3]),mapped), 'Palette index roundtrip changed'
                name=f'{job["id"]}-d{f["direction"]}-{f["frame"]:02}.png';path=target/name;Image.fromarray(corrected).save(path)
                shown=Image.fromarray(pal.show(corrected));shown.save(target/(name[:-4]+'-display.png'))
                old=Image.fromarray(pal.show(a));original_im=Image.fromarray(pal.show(np.asarray(Image.open(out/f['original']).convert('RGBA'))))
                w,h=original_im.size;native=shown.resize((w,h),Image.Resampling.BOX);native.save(target/(name[:-4]+'-native.png'))
                panel=Image.new('RGB',(w*16+32,h*4+26),(39,42,43));draw=ImageDraw.Draw(panel)
                draw.text((2,3),f'{job["id"]} d{f["direction"]}: source / previous / tone repair / native shrink',fill='white')
                for n,im in enumerate([original_im.resize((w*4,h*4),Image.Resampling.NEAREST),old,shown,native.resize((w*4,h*4),Image.Resampling.NEAREST)]):panel.paste(im,(n*(w*4+8),26),im)
                panels.append(panel);nf=dict(f);nf['hd']=str(path.relative_to(out));nf['native']=str((target/(name[:-4]+'-native.png')).relative_to(out));nf['sha256']=restore.digest(path.read_bytes());newframes.append(nf)
            updated=dict(report);updated['frames']=newframes;updated['status']='tone_candidate_needs_visual_and_runtime_review';updated['tone_input_report_sha256']=restore.digest(rp.read_bytes());updated['tone_receipt']=str((target/'receipt.json').relative_to(out))
            newrp=target/(job['id']+'-report.json');restore.save_json(newrp,updated)
            cols=min(2,len(panels));cw=max(p.width for p in panels);ch=max(p.height for p in panels);board=Image.new('RGB',(cols*cw,((len(panels)+cols-1)//cols)*ch),(39,42,43))
            for i,panel in enumerate(panels):board.paste(panel,((i%cols)*cw,(i//cols)*ch))
            board.save(target/(job['id']+'-contact.png'));panels[0].save(target/(job['id']+'-loop.gif'),save_all=True,append_images=panels[1:],duration=150,loop=0)
            records.append({'job':job['id'],'report':str(newrp.relative_to(out)),'sha256':restore.digest(newrp.read_bytes())})
        receipt['actions'].append({'action':action['action'],'curve':curve,'source':stats(src,pal),'before':stats(before,pal),'after':stats(expected_after,pal),'reports':records,'all_alpha_geometry_and_order_identical':True})
    restore.save_json(target/'receipt.json',receipt)
    print(json.dumps({'receipt':str(target/'receipt.json'),'actions':[{'action':a['action'],'source_L':a['source']['L_quantiles'],'before_L':a['before']['L_quantiles'],'after_L':a['after']['L_quantiles']} for a in receipt['actions']]}))

if __name__=='__main__':main()
