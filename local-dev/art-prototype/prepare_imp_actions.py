# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract remaining imp poses from local game data, retaining timing and anchors."""
import json
import math
import struct
from PIL import Image
from extract_imp_reference import ROOT, decode, indices, tab

OUT = ROOT / 'artifacts/art-prototype/imp-actions'
REF = OUT / 'references'
REF.mkdir(parents=True, exist_ok=True)
ACTIONS = [('hit',560),('relax',562),('claim',564),('drag',566),
           ('smoke',568),('scream',570),('death',572),('grab',574),('slap',576)]
records = {}
for action, fp in ACTIONS:
    records[action] = {'td_animation':fp+1,'fp_animation':fp,'td':[],'fp':[]}
    for view, anim in [('td',fp+1),('fp',fp)]:
        header = struct.unpack_from('<I8Bhh', tab, indices[anim]*16)
        directions = 5 if header[5] == 2 else 1
        count = header[6]
        records[action].update(count=count,rotatable=directions==5)
        for direction in range(directions):
            frames=[]; items=[]
            for n in range(count):
                frame,info=decode(indices[anim]+direction*count+n)
                name=f'{action}-{view}-d{direction}-{n+1:02}.png'
                frame.save(REF/name)
                info.update(file=name,animation=anim,direction=direction,frame=n)
                frames.append(frame);items.append(info)
            records[action][view].append(items)
            cols=min(count,4) if count!=6 else 3
            rows=math.ceil(count/cols)
            w,h=frames[0].size
            factor=min(16,1800//max(w*cols,h*rows))
            sheet=Image.new('RGBA',(w*cols*factor,h*rows*factor))
            for n,frame in enumerate(frames):
                sheet.alpha_composite(frame.resize((w*factor,h*factor),Image.Resampling.NEAREST),((n%cols)*w*factor,(n//cols)*h*factor))
            sheet.save(REF/f'{action}-{view}-d{direction}-reference.png')
    # Single/short pose actions share a direction sheet: rows front -> rear,
    # columns temporal frame. This is a visual reference, never runtime data.
    if count<=2 and directions==5:
        w,h=frames[0].size
        w,h=records[action]['td'][0][0]['frame_size']
        factor=10
        sheet=Image.new('RGBA',(w*count*factor,h*5*factor))
        for d,items in enumerate(records[action]['td']):
            for n,info in enumerate(items):
                im=Image.open(REF/info['file'])
                sheet.alpha_composite(im.resize((w*factor,h*factor),Image.Resampling.NEAREST),(n*w*factor,d*h*factor))
        sheet.save(REF/f'{action}-td-all-reference.png')
(OUT/'source-manifest.json').write_text(json.dumps(records,indent=2)+'\n')
print(json.dumps({a:{'frames':m['count'],'directions':len(m['td']),'total':m['count']*len(m['td'])} for a,m in records.items()}))
