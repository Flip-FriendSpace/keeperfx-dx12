# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply one source-derived display-palette colour fit to complete FP actions.

Reuses the existing accepted sprite colour fitter without its import side effects.
Never changes alpha, pose registration, original members, TD or held artwork.
"""
from pathlib import Path
import argparse, json, re
import numpy as np
from PIL import Image, ImageDraw
import restore


def colour_math(workspace):
    import colour_math as fitter
    path = Path(fitter.__file__)
    return vars(fitter), restore.digest(path.read_bytes())

class Palette:
    def __init__(self, workspace, math):
        data = workspace / 'runtime/keeperfx-x64/data'
        self.conv = np.frombuffer((data/'png_conv_pal.dat').read_bytes(),dtype=np.uint8).reshape(256,3)
        self.pal = np.frombuffer((data/'palette.dat').read_bytes(),dtype=np.uint8).reshape(256,3).astype(np.uint16)*4
        self.pal_lin = math['to_linear'](self.pal)
    def index(self,rgb):
        shape = rgb.shape[:-1]; q = np.asarray(rgb,dtype=np.uint8).reshape(-1,3)//4
        unique, inverse = np.unique(q,axis=0,return_inverse=True); result=[]
        for start in range(0,len(unique),4096):
            result.extend(np.abs(unique[start:start+4096,None,:].astype(np.int16)-self.conv[None].astype(np.int16)).sum(-1).argmin(1))
        return np.asarray(result,dtype=np.uint8)[inverse].reshape(shape)
    def show(self,rgba):
        out=rgba.copy();out[:,:,:3]=self.pal[self.index(rgba[:,:,:3])];return out

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--slug',required=True);ap.add_argument('--jobs',nargs='+',required=True)
    ap.add_argument('--report-files',nargs='+');ap.add_argument('--grade-from');ap.add_argument('--tag',required=True)
    ap.add_argument('--workspace',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True,help='Preparation directory passed to restore.py')
    args=ap.parse_args()
    if not __debug__:ap.error('Run without -O: source preservation assertions are required')
    if not re.fullmatch(r'[a-z0-9][a-z0-9_]*',args.slug):ap.error('Invalid creature slug')
    out=args.output.resolve()/args.slug
    assert re.fullmatch(r'[a-z0-9][a-z0-9-]*',args.tag), 'Invalid candidate tag'
    if args.slug == 'ghost': raise ValueError('Ghost requires its dedicated additive colour encoder')
    source=restore.read_json(out/'source.json');plan=restore.read_json(out/'generation-plan.json')
    restore.authenticate_source(out,source)
    for action in source['actions']:
        jobs=restore.action_jobs(plan,action)
        chosen={j['id'] for j in jobs}.intersection(args.jobs)
        assert not chosen or chosen=={j['id'] for j in jobs}, 'Colour grade requires a complete action'
    target=out/'colour'/args.tag
    if target.exists():raise ValueError('Refusing existing colour candidate')
    report_paths=[out/p for p in args.report_files] if args.report_files else [out/f'review/{job}-report.json' for job in args.jobs]
    assert len(report_paths)==len(args.jobs)
    assert all(p.resolve().is_relative_to(out.resolve()) for p in report_paths), 'Report escapes candidate directory'
    reports=[restore.read_json(p) for p in report_paths]
    assert all(r['source_sha256']==restore.digest((out/'source.json').read_bytes()) for r in reports), 'Stale source report'
    assert all(r['job']==job for r,job in zip(reports,args.jobs))
    for job_id, report in zip(args.jobs,reports):
        job=next(j for j in plan['jobs'] if j['id']==job_id)
        action=next(a for a in source['actions'] if a['action']==job['action'])
        assert sorted((f['direction'],f['frame']) for f in report['frames'])==sorted(tuple(c) for c in job['cells']), 'Incomplete colour input sequence'
        for f in report['frames']:
            assert f['original']==action['sequences'][f['direction']][f['frame']]['file'], 'Wrong original pose reference'
    frames=[f for r in reports for f in r['frames']]
    math,math_hash=colour_math(args.workspace);pal=Palette(args.workspace,math)
    original=[];generated=[];inputs=[]
    for f in frames:
        rgba_path=out/f['rgba']
        assert rgba_path.resolve().is_relative_to(out.resolve()), 'RGBA escapes candidate directory'
        rgba_hash=restore.digest(rgba_path.read_bytes())
        assert not f.get('rgba_sha256') or f['rgba_sha256']==rgba_hash, 'Changed raw candidate'
        inputs.append({'rgba':f['rgba'],'rgba_sha256':rgba_hash,'original':f['original'],'original_sha256':restore.digest((out/f['original']).read_bytes())})
        raw=np.asarray(Image.open(out/f['original']).convert('RGBA'));shown=pal.show(raw)
        rgb=shown[:,:,:3][shown[:,:,3]>=64];original.append(rgb)
        hd=np.asarray(Image.open(out/f['rgba']).convert('RGBA'));generated.append((hd,4))
    original=np.concatenate(original);linear=math['to_linear'](original)
    target_stats=(linear.sum(0),len(linear),math['chroma'](linear).sum(),len(linear))
    sample=math['Sample'](pal,generated)
    grade_source=None
    if args.grade_from:
        grade_path=out/args.grade_from
        assert grade_path.resolve().is_relative_to(out.resolve()), 'Grade receipt escapes candidate directory'
        grade=restore.read_json(grade_path)['grade']
        grade_source={'file':args.grade_from,'sha256':restore.digest(grade_path.read_bytes())}
    else: grade=math['fit'](target_stats,sample.measure,16)
    target.mkdir(parents=True)
    receipt={'schema':1,'jobs':args.jobs,'tag':args.tag,'method':'One fixed linear-light RGB gain and CIELAB chroma scale across every selected direction/frame after actual engine palette conversion. Fitted from source or reused from the named receipt; no frame-specific normalization.',
        'fitter_sha256':math_hash,'grade_source':grade_source,'inputs':inputs,'grade':grade,'target_summary':math['summary'](target_stats).tolist(),
        'before_summary':math['summary'](sample.measure({'gains':[1,1,1],'chroma_scale':1})).tolist(),
        'after_summary':math['summary'](sample.measure(grade)).tolist(),'reports':[],
        'visual_review':'pending','runtime':'not_run','protected_existing_art':'unchanged'}
    for job,report,report_path in zip(args.jobs,reports,report_paths):
        changed=[];panels=[]
        for f in report['frames']:
            raw=np.asarray(Image.open(out/f['rgba']).convert('RGBA'));graded=raw.copy()
            graded[:,:,:3]=math['grade'](raw[:,:,:3],grade)
            index=pal.index(graded[:,:,:3]);graded[:,:,:3]=(pal.conv.astype(np.uint16)*4)[index]
            graded[:,:,3]=np.where(raw[:,:,3]>=64,255,0)
            assert np.array_equal(graded[:,:,3],np.where(raw[:,:,3]>=64,255,0))
            name=f'{job}-d{f["direction"]}-{f["frame"]:02}.png';path=target/name;Image.fromarray(graded).save(path)
            original=np.asarray(Image.open(out/f['original']).convert('RGBA'));original=Image.fromarray(pal.show(original))
            shown=Image.fromarray(pal.show(graded));native=shown.resize(original.size,Image.Resampling.BOX)
            native_path=target/(name[:-4]+'-native.png');native.save(native_path)
            shown.save(target/(name[:-4]+'-display.png'))
            w,h=original.size
            panel=Image.new('RGB',(w*12+24,h*4+25),(39,42,43));draw=ImageDraw.Draw(panel)
            draw.text((2,3),f'{job} d{f["direction"]} f{f["frame"]}: source / detailed / native shrink',fill='white')
            for i,img in enumerate([original.resize((w*4,h*4),Image.Resampling.NEAREST),shown,native.resize((w*4,h*4),Image.Resampling.NEAREST)]):
                panel.paste(img,(i*(w*4+8),25),img)
            panels.append(panel)
            new=dict(f);new['hd']=str(path.relative_to(out));new['native']=str(native_path.relative_to(out));new['sha256']=restore.digest(path.read_bytes());changed.append(new)
        updated=dict(report);updated['frames']=changed;updated['colour_receipt']=str((target/'receipt.json').relative_to(out));updated['status']='colour_candidate_needs_visual_review'
        updated['ungraded_report_sha256']=restore.digest(report_path.read_bytes())
        restore.save_json(target/(job+'-report.json'),updated)
        cols=min(2,len(panels));cw=max(p.width for p in panels);ch=max(p.height for p in panels)
        board=Image.new('RGB',(cols*cw,((len(panels)+cols-1)//cols)*ch),(39,42,43))
        for i,panel in enumerate(panels):board.paste(panel,((i%cols)*cw,(i//cols)*ch))
        board.save(target/(job+'-contact.png'))
        panels[0].save(target/(job+'-loop.gif'),save_all=True,append_images=panels[1:],duration=150,loop=0)
        receipt['reports'].append({'job':job,'report':str((target/(job+'-report.json')).relative_to(out)),'sha256':restore.digest((target/(job+'-report.json')).read_bytes())})
    restore.save_json(target/'receipt.json',receipt);print(json.dumps({'receipt':str(target/'receipt.json'),'jobs':args.jobs,'frames':len(frames),'grade':grade,'target_summary':receipt['target_summary'],'after_summary':receipt['after_summary']}))

if __name__=='__main__':main()
