# SPDX-License-Identifier: GPL-2.0-or-later
"""Add optional combined single-pose strips without rewriting artist plans."""
from pathlib import Path
import json
from PIL import Image
ROOT=Path(__file__).resolve().parents[3]/'artifacts/art-prototype/heroes'
for folder in ROOT.iterdir():
    path=folder/'source-manifest.json'
    if not path.is_file():continue
    meta=json.loads(path.read_text())
    for action,m in meta.items():
        if len(m['td'])!=5 or any(len(seq)!=1 for seq in m['td']):continue
        images=[Image.open(folder/'references'/seq[0]['file']).convert('RGBA') for seq in m['td']]
        w=max(im.width for im in images);h=max(im.height for im in images);f=min(12,1800//max(w*5,h))
        strip=Image.new('RGBA',(w*5*f,h*f))
        for d,im in enumerate(images):strip.alpha_composite(im.resize((im.width*f,im.height*f),Image.Resampling.NEAREST),(d*w*f,0))
        strip.save(folder/'references'/f'{action}-td-all-reference.png')
