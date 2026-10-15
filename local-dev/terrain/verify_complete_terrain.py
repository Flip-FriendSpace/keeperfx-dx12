# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify complete terrain coverage and actual animation bindings in saved receipts."""
import argparse,hashlib,json,tomllib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--before',type=Path,required=True);p.add_argument('--after',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--animations',type=Path,required=True);a=p.parse_args()
def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))
before,after=read(a.before/'fixture-report.json'),read(a.after/'fixture-report.json')
br,ar=read(a.before/'run-report.json'),read(a.after/'run-report.json')
manifest=read(a.manifest)
package_sha=manifest.get('runtime_pack_sha256') or manifest['package']['sha256']
tiles=[t['tile'] for t in manifest['tiles']] if 'tiles' in manifest else manifest['package']['tiles']
assert before['passed'] and after['passed'] and br['passed'] and ar['passed']
assert br['settingsRestored'] and ar['settingsRestored'] and br['apiOwnerVerified'] and ar['apiOwnerVerified']
assert br['executableSHA256']==ar['executableSHA256']
assert ar['packageSHA256'].lower()==package_sha
assert set(ar['dx12']['matchedTiles'])==set(tiles) and not br['dx12']['matchedTiles']
assert br['dx12']['rejected']==ar['dx12']['rejected']==0
bs={s['id']:s for s in before['stages']};hs={s['id']:s for s in after['stages']}
assert bs.keys()==hs.keys() and len(bs)==32
for name in bs:
 assert bs[name]['camera']==hs[name]['camera'] and bs[name]['columns']==hs[name]['columns'],name
animations=tomllib.loads(a.animations.read_text(encoding='utf-8-sig'))
phase_checks=[];first=None
def physical(offset):
 off=offset%(1088*1024);index=(off//8192)*8+(off%256)//32
 return index if index<544 else 1000+index-544
for phase in range(9):
 name='animation-'+str(phase) if phase<8 else 'animation-loop'
 b=read(a.before/bs[name]['receipt'])['surface'];h=read(a.after/hs[name]['receipt'])['surface']
 bf,hf=b['textureFrames'],h['textureFrames']
 assert len(bf)==len(hf)==32*1544
 assert b['textureGeneration']==h['textureGeneration']
 phase_sources=[]
 for alias in range(544,605):
  expected=animations['texture'+str(alias)]['frames'][phase%8]
  for variation in range(32):
   i=variation*1544+alias
   assert bf[i]==hf[i] and physical(hf[i])==expected,(phase,alias,variation,expected,physical(hf[i]))
  phase_sources.append(expected)
 if phase==0:first=phase_sources
 if phase==8:assert first==phase_sources
 phase_checks.append({'stage':name,'turn':hs[name]['turn'],'source_generation':h['textureGeneration'],'aliases':61,'variations':32,'physical_sources':phase_sources})
assert len({tuple(c['physical_sources']) for c in phase_checks[:8]})==8
s=after['finalStats']
assert s['validation']=={'errors':0,'warnings':0}
for key,budget in [('cpuHighWater','cpuBudgetBytes'),('defaultHighWater','defaultBudgetBytes'),('uploadHighWater','uploadBudgetBytes'),('producerHighWater','producerBudgetBytes')]:assert s['memory'][key]<=s['memory'][budget]
assert s['descriptors']['highWater']<=s['descriptors']['budget']
result={'passed':True,'before':str(a.before.resolve()),'after':str(a.after.resolve()),'package_sha256':ar['packageSHA256'].lower(),'executable_sha256':ar['executableSHA256'].lower(),'matched_stages':32,'matched_replacements':601,'phase_alias_variation_checks':9*61*32,'animation_loop_closed':True,'animation_phases':phase_checks,'memory':s['memory'],'descriptors':s['descriptors'],'broader_gpu_stress_qualification':'open - immutable page COPY_DEST validation error 1358, retained separately','artistic_acceptance':'601 enhanced tiles accepted by the user; 31 original cells retained','scope':'Live material/camera transitions plus complete physical source gallery and actual eight-phase alias bindings. Not whole-campaign, distance shimmer, door-gameplay or full renderer qualification.'}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:result[k] for k in ['passed','matched_stages','matched_replacements','phase_alias_variation_checks','animation_loop_closed']}))
