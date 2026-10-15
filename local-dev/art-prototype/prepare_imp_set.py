# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract all original idle/walk/dig directions, retaining native anchors."""
import json
from pathlib import Path
from PIL import Image
from extract_imp_reference import decode, indices, ROOT

OUT = ROOT / 'artifacts/art-prototype/imp-set'
REF = OUT / 'references'
REF.mkdir(parents=True, exist_ok=True)
records = {}
for action, td, fp, count in [('idle',557,556,1),('walk',555,554,6),('dig',559,558,6)]:
    records[action] = {'td_animation':td,'fp_animation':fp,'count':count,'td':[],'fp':[]}
    for view, anim in [('td',td),('fp',fp)]:
        for direction in range(5):
            direction_records = []
            frames = []
            for number in range(count):
                frame, info = decode(indices[anim] + direction*count + number)
                name = f'{action}-{view}-d{direction}-{number+1:02}.png'
                frame.save(REF / name)
                info.update(file=name,animation=anim,direction=direction,frame=number)
                direction_records.append(info)
                frames.append(frame)
            records[action][view].append(direction_records)
            if view == 'td':
                cols,rows = (1,1) if count == 1 else (3,2)
                w,h = frames[0].size
                assert all(f.size==(w,h) for f in frames)
                factor = 24 if count == 1 else 12
                sheet=Image.new('RGBA',(w*cols*factor,h*rows*factor))
                for n,f in enumerate(frames):
                    sheet.alpha_composite(f.resize((w*factor,h*factor),Image.Resampling.NEAREST),((n%cols)*w*factor,(n//cols)*h*factor))
                sheet.save(REF / f'{action}-d{direction}-reference.png')

# Four remaining idle directions in a 2x2 image; d1 already has an accepted image.
idle=Image.new('RGBA',(35*20*2,31*20*2))
for n,d in enumerate([0,2,3,4]):
    frame=Image.open(REF/f'idle-td-d{d}-01.png')
    idle.alpha_composite(frame.resize((700,620),Image.Resampling.NEAREST),((n%2)*700,(n//2)*620))
idle.save(REF/'idle-remaining-reference.png')
(OUT/'source-manifest.json').write_text(json.dumps(records,indent=2)+'\n')
print(json.dumps({'root':str(OUT),'reference_frames':130,'actions':list(records)}))
