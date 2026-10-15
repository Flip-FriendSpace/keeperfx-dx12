# SPDX-License-Identifier: GPL-2.0-or-later
"""Package generated sprite artwork using the engine's original frame geometry.

Mechanical format conversion only: component-aware slicing, common canvas resampling,
and exact engine palette/alpha conversion. Original/source artwork is retained.
"""
from pathlib import Path
import hashlib
import io
import json
import zipfile
import numpy as np
from PIL import Image, ImageFilter

ROOT=Path(__file__).resolve().parents[3]
ART=ROOT/'artifacts/art-prototype'
OUT=ART/'imp-set'
META=json.loads((OUT/'source-manifest.json').read_text())
palette=np.frombuffer((ROOT/'runtime/keeperfx-x64/data/png_conv_pal.dat').read_bytes(),dtype=np.uint8).reshape(256,3)
palette_rgb=(palette.astype(np.uint16)*4).astype(np.uint8)
frames_root=OUT/'frames'
frames_root.mkdir(exist_ok=True)
mod=OUT/'mod/imp_art_prototype'
(mod/'fxdata').mkdir(parents=True,exist_ok=True)
(mod/'creatrs').mkdir(exist_ok=True)

def split(path,cols,rows,required_cells=None):
    im=Image.open(path).convert('RGBA')
    w,h=im.size
    # Label opaque connected components by row runs. Whole tools can cross a
    # nominal cell boundary; ownership follows the connected sprite, not a cut.
    opaque=np.asarray(im)[:,:,3]>=64
    labels=np.zeros((h,w),dtype=np.int32);parent=[0];previous=[]
    def root(n):
        while parent[n]!=n:
            parent[n]=parent[parent[n]];n=parent[n]
        return n
    for y,row in enumerate(opaque):
        changes=np.diff(np.pad(row.astype(np.int8),(1,1)))
        current=[];j=0
        for x0,x1 in zip(np.flatnonzero(changes==1),np.flatnonzero(changes==-1)):
            n=len(parent);parent.append(n)
            while j<len(previous) and previous[j][1]<x0:j+=1
            k=j
            while k<len(previous) and previous[k][0]<=x1:
                parent[root(previous[k][2])]=root(n);k+=1
            labels[y,x0:x1]=n;current.append((x0,x1,n))
        previous=current
    lookup=np.array([root(n) for n in range(len(parent))]);labels=lookup[labels]
    counts=np.bincount(labels.ravel());groups=[[] for _ in range(cols*rows)]
    for n in np.flatnonzero(counts>=16):
        if n==0:continue
        ys,xs=np.nonzero(labels==n)
        cx=(xs.min()+xs.max())/2;cy=(ys.min()+ys.max())/2
        cell=min(rows-1,int(cy/h*rows))*cols+min(cols-1,int(cx/w*cols))
        groups[cell].append(int(n))
    cw,ch=round(w/cols),round(h/rows);pad=max(cw,ch)//3
    result=[]
    for i,group in enumerate(groups):
        # Hero sheets can end with deliberately unused cells (e.g. 5 in 4x2).
        # Preserve strict missing-cell rejection for all requested frames.
        if not group and required_cells is not None and i not in required_cells:
            result.append(None)
            continue
        assert group, f'Missing cell {i} in {path}'
        mask=Image.fromarray(np.isin(labels,group).astype(np.uint8)*255).filter(ImageFilter.MaxFilter(5))
        cut=im.copy();cut.putalpha(Image.fromarray(np.minimum(np.asarray(im)[:,:,3],np.asarray(mask))))
        b=cut.getbbox();tile=Image.new('RGBA',(cw+2*pad,ch+2*pad))
        origin=(round((i%cols)*w/cols),round((i//cols)*h/rows))
        tile.paste(cut.crop(b),(b[0]-origin[0]+pad,b[1]-origin[1]+pad))
        result.append(tile)
    return result

def bounds(image):
    return Image.fromarray((np.asarray(image)[:,:,3]>=64).astype(np.uint8)*255).getbbox()

def register_sequence(images,infos,reference_root=None):
    # One affine transform for the entire sequence. No frame-by-frame fitting,
    # which would turn a moving pickaxe into a changing body size.
    boxes=[bounds(im) for im in images]
    union=(min(b[0] for b in boxes),min(b[1] for b in boxes),max(b[2] for b in boxes),max(b[3] for b in boxes))
    originals=[Image.open((reference_root or OUT/'references')/info['file']).convert('RGBA') for info in infos]
    ob=[bounds(im) for im in originals]
    target=(min(b[0] for b in ob),min(b[1] for b in ob),max(b[2] for b in ob),max(b[3] for b in ob))
    fw,fh=infos[0]['frame_size']
    target=(max(1,target[0]),max(1,target[1]),min(fw-1,target[2]),min(fh-1,target[3]))
    scale=min((target[2]-target[0])/(union[2]-union[0]),(target[3]-target[1])/(union[3]-union[1]))
    dx=(target[0]+target[2])/2-(union[0]+union[2])*scale/2
    dy=target[3]-union[3]*scale
    result=[]
    for im,info in zip(images,infos):
        size=tuple(v*4 for v in info['frame_size'])
        large=im.resize((round(im.width*scale*4),round(im.height*scale*4)),Image.Resampling.LANCZOS)
        canvas=Image.new('RGBA',size);canvas.paste(large,(round(dx*4),round(dy*4)))
        result.append(canvas)
    return result

def engine_png(image):
    rgba=np.array(image)
    colors=rgba[:,:,:3]//4
    unique,inverse=np.unique(colors.reshape(-1,3),axis=0,return_inverse=True)
    mapped=[]
    for start in range(0,len(unique),4096):
        delta=np.abs(unique[start:start+4096,None,:].astype(np.int16)-palette[None,:,:].astype(np.int16)).sum(axis=2)
        mapped.extend(delta.argmin(axis=1).tolist())
    indices=np.asarray(mapped,dtype=np.uint8)[inverse].reshape(rgba.shape[:2])
    rgba[:,:,:3]=palette_rgb[indices]
    rgba[:,:,3]=np.where(rgba[:,:,3]>=64,255,0)
    return Image.fromarray(rgba)

if __name__ == '__main__':
    idle_other=dict(zip([0,2,3,4],split(OUT/'idle-remaining-sheet.png',2,2)))
    sources={};review={};checks=[]
    for action,meta in META.items():
        sources[action]=[];review[action]=[]
        for direction in range(5):
            if action=='idle':
                source=[Image.open(ART/'imp-overhead-v1.png').convert('RGBA')] if direction==1 else [idle_other[direction]]
            elif action=='walk' and direction==1:
                source=split(ART/'imp-walk-sheet-v1.png',3,2)
            else:
                source=split(OUT/f'{action}-d{direction}-sheet.png',3,2)
            if action=='dig' and direction==0:
                sheet=Image.open(OUT/'dig-d0-sheet.png');cw,ch=round(sheet.width/3),round(sheet.height/2);pad=max(cw,ch)//3
                corrected=Image.open(OUT/'dig-d0-raised-corrected.png').convert('RGBA').resize((cw,ch),Image.Resampling.LANCZOS)
                source[1]=Image.new('RGBA',source[1].size);source[1].paste(corrected,(pad,pad))
            source=register_sequence(source,meta['td'][direction])
            sources[action].append(source)
            direction_review=[]
            for n,(image,info) in enumerate(zip(source,meta['td'][direction])):
                native_w,native_h=info['frame_size']
                hd=image
                native=image.resize((native_w,native_h),Image.Resampling.LANCZOS)
                basename=f'{action}-d{direction}-{n+1:02}'
                hd.save(frames_root/f'{basename}-rgba.png')
                native_engine=engine_png(native);hd_engine=engine_png(hd)
                native_engine.save(frames_root/f'{basename}-native.png')
                hd_engine.save(frames_root/f'{basename}-hd.png')
                b=hd_engine.getchannel('A').getbbox()
                assert b is not None
                checks.append({'action':action,'direction':direction,'frame':n+1,'native':[native_w,native_h],
                               'hd':list(hd.size),'hd_bounds':b,'touches_edge':b[0]==0 or b[1]==0 or b[2]==hd.width or b[3]==hd.height})
                direction_review.append({'original':f'references/{info["file"]}','rgba':f'frames/{basename}-rgba.png',
                                         'native':f'frames/{basename}-native.png','hd':f'frames/{basename}-hd.png',
                                         'size':[native_w,native_h],'anchor':[-info['anchor'][0],-info['anchor'][1]]})
            review[action].append(direction_review)

    assert not any(c['touches_edge'] for c in checks), 'A packaged sprite reaches the frame boundary'

    entries=[]
    zip_path=mod/'fxdata/imp-sprites.zip'
    with zipfile.ZipFile(zip_path,'w',zipfile.ZIP_DEFLATED) as archive:
        for factor,tag in [(1,'NATIVE'),(4,'HD')]:
            for action,meta in META.items():
                entry={'name':f'ARTIMP_{tag}_{action.upper()}','rotatable':True,
                       'td_resolution_scale':factor,'fp_resolution_scale':1,'td':[],'fp':[]}
                for view in ['td','fp']:
                    for direction in range(5):
                        seq=[]
                        for n,info in enumerate(meta[view][direction]):
                            if view=='td':
                                image=Image.open(frames_root/f'{action}-d{direction}-{n+1:02}-{tag.lower()}.png')
                            else:
                                # Keep the original possession sprites for the local prototype.
                                image=Image.open(OUT/'references'/info['file']).convert('RGBA')
                            assert image.width<255 and image.height<255
                            filename=f'{tag.lower()}/{action}/{view}/d{direction}-{n+1:02}.png'
                            buffer=io.BytesIO();image.save(buffer,format='PNG');archive.writestr(filename,buffer.getvalue())
                            view_factor=factor if view=='td' else 1
                            seq.append({'file':filename,'frame_w':image.width,'frame_h':image.height,
                                        'offset_x':-info['anchor'][0]*view_factor,'offset_y':-info['anchor'][1]*view_factor,
                                        'shadow_offset':0,
                                        'offset_w':0,'offset_h':0})
                        entry[view].append(seq)
                entries.append(entry)
        archive.writestr('sprites.json',json.dumps(entries,separators=(',',':')))
    (mod/'creatrs/imp.cfg').write_text('[sprites]\nStand = ARTIMP_HD_IDLE\nAmbulate = ARTIMP_HD_WALK\nDig = ARTIMP_HD_DIG\n')
    (mod/'local-prototype.json').write_text(json.dumps({'local_only':True,'overhead_states':['idle','walk','dig'],
        'possession':'Original local game frames retained at 1x','hd':'4x overhead frames with resolution metadata; normal creature size',
        'commercial_source_assets':'Do not publish this local package'},indent=2)+'\n')
    (OUT/'review-data.json').write_text(json.dumps(review,separators=(',',':')))
    (OUT/'review-data.js').write_text('window.impReviewData='+json.dumps(review,separators=(',',':'))+';\n')
    report={'overhead_frames':len(checks),'directions':5,'states':{'idle':1,'walk':6,'dig':6},
            'sprite_entries':len(entries),'runtime_pngs':260,'zip_sha256':hashlib.sha256(zip_path.read_bytes()).hexdigest(),
            'palette':'Engine RGB/4 Manhattan nearest colour, first-index tie break; alpha >=64 opaque',
            'geometry':'Connected components retain whole tools across grid boundaries. One uniform transform per direction/action fits the original sequence bounds; unchanged native anchors, no per-frame fitting.',
            'correction':'Front digging frame 2 uses the separately generated edge-on pickaxe head correction',
            'touching_edges':[c for c in checks if c['touches_edge']], 'frames':checks}
    (OUT/'package-report.json').write_text(json.dumps(report,indent=2)+'\n')
    with zipfile.ZipFile(zip_path) as archive:
        assert archive.testzip() is None
        parsed=json.loads(archive.read('sprites.json'))
        assert len(parsed)==6
        for e in parsed:
            assert len(e['td'])==len(e['fp'])==5
            assert len({len(s) for s in e['td']+e['fp']})==1
    print(json.dumps({k:report[k] for k in ['overhead_frames','sprite_entries','runtime_pngs','zip_sha256','touching_edges']}))
