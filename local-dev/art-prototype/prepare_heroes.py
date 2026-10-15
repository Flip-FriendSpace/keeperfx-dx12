# SPDX-License-Identifier: GPL-2.0-or-later
"""Local-only original hero references and explicit generation plans.

Load decoder definitions through AST to avoid its original-imp import writes.
"""
from pathlib import Path
import ast
import json
import math
import re
import struct
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
OUT = ROOT / 'artifacts/art-prototype/heroes'
ROSTER = 'wizard barbarian archer monk dwarfa knight avatar tunneller witch giant fairy thief samurai'.split()
NAMES = {'Stand':'idle', 'Ambulate':'walk', 'Attack':'attack', 'GotHit':'hit', 'PowerGrab':'grab', 'GotSlapped':'slap', 'DropDead':'death', 'PrettyDance':'celebrate', 'EatChicken':'eat', 'Scream':'scream'}
SHARED = {'Torture','DeadSplat','QuerySymbol','HandSymbol'}

def decoder():
    tree = ast.parse((HERE/'extract_imp_reference.py').read_text())
    node = next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='decode')
    source = (ROOT/'keeperfx/src/creature_graphics.c').read_text()
    array = re.search(r'creature_list\[CREATURE_FRAMELIST_LENGTH\]\s*=\s*\{(.*?)\};',source,re.S).group(1)
    indices = [int(n) for n in re.findall(r'\d+',re.sub(r'/\*.*?\*/|//[^\n]*','',array,flags=re.S))]
    data = ROOT/'runtime/keeperfx-x64/data'
    pal = (data/'palette.dat').read_bytes()
    env = {'struct':struct,'Image':Image,'tab':(data/'creature.tab').read_bytes(),'blob':(data/'creature.jty').read_bytes(),'palette':[tuple(v*4 for v in pal[i:i+3])+(255,) for i in range(0,768,3)]}
    exec(compile(ast.Module(body=[node],type_ignores=[]),str(HERE/'extract_imp_reference.py'),'exec'),env)
    return env['decode'],env['tab'],indices

def config(slug):
    text = (ROOT/f'keeperfx/config/creatrs/{slug}.cfg').read_text()
    section = text.split('[sprites]')[1].split('[')[0]
    return {k:int(v) for k,v in re.findall(r'^\s*(\w+)\s*=\s*(\d+)\s*$',section,re.M)}

def main():
    decode,tab,indices = decoder()
    inventory = []
    for slug in ROSTER:
        dest=OUT/slug; refs=dest/'references'; refs.mkdir(parents=True,exist_ok=True)
        aliases=config(slug); actions={}; mapping={}; shared={}; jobs=[]; blockers=[]
        for alias,fp in aliases.items():
            if alias in SHARED:
                shared[alias]=fp; continue
            existing=next((a for a,m in actions.items() if m['fp_animation']==fp),None)
            action=existing or NAMES.get(alias,alias.lower())
            mapping[alias]=action
            if existing: continue
            meta={'fp_animation':fp,'td_animation':fp+1,'td':[],'fp':[]}
            actions[action]=meta
            for view,anim in [('td',fp+1),('fp',fp)]:
                header=struct.unpack_from('<I8Bhh',tab,indices[anim]*16)
                dirs=5 if header[5]==2 else 1; count=header[6]
                for d in range(dirs):
                    frames=[]; infos=[]
                    for n in range(count):
                        frame,info=decode(indices[anim]+d*count+n)
                        name=f'{action}-{view}-d{d}-{n+1:02}.png'; frame.save(refs/name)
                        info.update(file=name,animation=anim,direction=d,frame=n)
                        frames.append(frame);infos.append(info)
                        if view=='td' and max(frame.size)*4>=255:
                            blockers.append({'action':action,'direction':d,'frame':n,'native':list(frame.size),'required_4x':[v*4 for v in frame.size]})
                    meta[view].append(infos)
                    cols=3 if count==6 else min(count,4);rows=math.ceil(count/cols)
                    w=max(f.width for f in frames);h=max(f.height for f in frames)
                    factor=max(1,min(12,1800//max(w*cols,h*rows)))
                    sheet=Image.new('RGBA',(w*cols*factor,h*rows*factor))
                    for n,f in enumerate(frames):sheet.alpha_composite(f.resize((f.width*factor,f.height*factor),Image.Resampling.NEAREST),((n%cols)*w*factor,(n//cols)*h*factor))
                    ref=f'references/{action}-{view}-d{d}-reference.png';sheet.save(dest/ref)
                    if view=='td': jobs.append({'action':action,'view':view,'direction':d,'count':count,'cols':cols,'rows':rows,'reference':ref,'output':f'{action}-td-d{d}-sheet.png','status':'pending_generation'})
            meta.update(count=len(meta['td'][0]),rotatable=len(meta['td'])==5)
        idle=actions[mapping['Stand']]['td'];w=max(i[0]['frame_size'][0] for i in idle);h=max(i[0]['frame_size'][1] for i in idle)
        factor=min(12,1800//max(w*len(idle),h));strip=Image.new('RGBA',(w*len(idle)*factor,h*factor))
        for d,infos in enumerate(idle):
            im=Image.open(refs/infos[0]['file']);strip.alpha_composite(im.resize((im.width*factor,im.height*factor),Image.Resampling.NEAREST),(d*w*factor,0))
        strip.save(refs/'idle-td-all-reference.png')
        (dest/'source-manifest.json').write_text(json.dumps(actions,indent=2)+'\n')
        plan={'schema':1,'slug':slug,'aliases':mapping,'shared_original':shared,'jobs':jobs,'idle_identity_reference':'references/idle-td-all-reference.png','blockers_4x':blockers,'status':'references_ready'}
        (dest/'generation-plan.json').write_text(json.dumps(plan,indent=2)+'\n')
        inventory.append({'slug':slug,'unique_actions':len(actions),'td_frames':sum(len(s) for m in actions.values() for s in m['td']),'actions':{a:{'frames':m['count'],'directions':len(m['td']),'frame_size':m['td'][0][0]['frame_size']} for a,m in actions.items()},'blockers_4x':blockers})
        print(json.dumps(inventory[-1]),flush=True)
    (OUT/'inventory.json').write_text(json.dumps(inventory,indent=2)+'\n')

if __name__=='__main__':main()
