# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the immutable camera/terrain inputs and images of two local fixtures."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument('--before', type=Path, required=True)
parser.add_argument('--after', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--before-label', default='Original Dungeon Keeper')
parser.add_argument('--after-label', default='HD terrain pilot')
parser.add_argument('--name', default='in-game-comparison')
parser.add_argument('--stages', nargs='+', help='Explicit matching stages; default checks all.')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

def load(folder):
    report = json.loads((folder / 'fixture-report.json').read_text(encoding='utf-8-sig'))
    assert report['passed'], 'The fixture did not pass.'
    return {stage['id']: stage for stage in report['stages']}

before, after = load(args.before), load(args.after)
checks = []
selected = args.stages or list(before)
for name in selected:
    assert name in after, name
    a, b = before[name], after[name]
    assert a['camera'] == b['camera'], f'Camera changed: {name}'
    assert a['columns'] == b['columns'], f'Terrain changed: {name}'
    checks.append({'stage': name, 'cameraEqual': True, 'columnsEqual': True,
                   'lightingComparison': 'observation_only' if name == 'normal-lighting' else 'fullbright'})
font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 22)
small = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 17)

def panels(name, crop, width):
    result = []
    for folder, report in ((args.before, before), (args.after, after)):
        im = Image.open(folder / report[name]['image']).convert('RGB').crop(crop)
        result.append(im.resize((width, round(im.height * width / im.width)), Image.Resampling.LANCZOS))
    return result

for name, suffix in [('overhead', ''), ('normal-lighting', '-normal-lighting')]:
    if name not in before or name not in selected:
        continue
    pair = panels(name, (425, 285, 1815, 1060), 850)
    board = Image.new('RGB', (1724, pair[0].height + 100), (22, 23, 26))
    draw = ImageDraw.Draw(board)
    draw.text((12, 10), args.before_label, font=font, fill='white')
    draw.text((874, 10), args.after_label, font=font, fill='white')
    board.paste(pair[0], (12, 48)); board.paste(pair[1], (874, 48))
    note = ('Matched camera and terrain; even diagnostic lighting.' if not suffix else
            'Normal-lighting observation: cursor position and randomized torches differ between runs.')
    draw.text((12, board.height - 32), note, font=small, fill=(200, 200, 200))
    board.save(args.output / (args.name + suffix + '.png'))

names = [name for name in ['overhead', 'rotated', 'short-walls', 'front', 'dig-to-path', 'rebuild-wall'] if name in selected]
rows = [panels(name, (376, 0, 1920, 1080), 770) for name in names]
height = rows[0][0].height
board = Image.new('RGB', (1564, (height + 44) * len(rows) + 48), (22, 23, 26))
draw = ImageDraw.Draw(board)
draw.text((12, 8), args.before_label, font=font, fill='white')
draw.text((794, 8), args.after_label, font=font, fill='white')
for row, (name, pair) in enumerate(zip(names, rows)):
    y = 48 + row * (height + 44)
    draw.text((12, y + 3), name, font=small, fill='white')
    board.paste(pair[0], (12, y + 32)); board.paste(pair[1], (794, y + 32))
board.save(args.output / (args.name + '-stages.png'))
# Expanded terrain fixtures get material and source-gallery review boards too.
for suffix, wanted in [('materials',['materials-0','materials-0-rotated','materials-1','materials-1-rotated']),('gallery',['gallery-0','gallery-192','gallery-256','gallery-320','gallery-384','gallery-448','gallery-512','gallery-576'])]:
    available=[n for n in wanted if n in selected]
    if not available: continue
    width=770 if suffix=='materials' else 600
    content=[panels(n,(376,0,1920,1080),width) for n in available]
    h=content[0][0].height
    montage=Image.new('RGB',(width*2+36,(h+42)*len(content)+48),(22,23,26))
    d=ImageDraw.Draw(montage);d.text((12,8),args.before_label,font=font,fill='white');d.text((width+24,8),args.after_label,font=font,fill='white')
    for i,(name,pair) in enumerate(zip(available,content)):
        y=48+i*(h+42);d.text((12,y),name,font=small,fill='white');montage.paste(pair[0],(12,y+30));montage.paste(pair[1],(width+24,y+30))
    montage.save(args.output/(args.name+'-'+suffix+'.png'))
if all('animation-'+str(i) in selected for i in range(8)):
    frames=[]
    for i in range(8):
        pair=panels('animation-'+str(i),(425,285,1815,1060),600)
        frame=Image.new('RGB',(1236,pair[0].height+70),(22,23,26));d=ImageDraw.Draw(frame)
        d.text((12,5),'Original - phase '+str(i),font=font,fill='white');d.text((624,5),'4x - phase '+str(i),font=font,fill='white')
        frame.paste(pair[0],(12,40));frame.paste(pair[1],(624,40));frames.append(frame)
    frames[0].save(args.output/(args.name+'-animations.gif'),save_all=True,append_images=frames[1:],duration=200,loop=0)
(args.output / (args.name + '-checks.json')).write_text(json.dumps({
    'before': str(args.before), 'after': str(args.after), 'checks': checks,
    'scope': 'Matched static camera/terrain inputs; not a whole-image pixel-equality test.'
}, indent=2) + '\n')
print(json.dumps({'output': str(args.output), 'name': args.name, 'matched_stages': len(checks)}))
