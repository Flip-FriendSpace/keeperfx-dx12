# SPDX-License-Identifier: GPL-2.0-or-later
"""Export enhanced terrain pixels and native fingerprints without native pixels."""
import argparse,hashlib,json,struct
from pathlib import Path
from PIL import Image
def digest(data):return hashlib.sha256(data).hexdigest()
def export(source,destination):
 m=json.loads((source/'manifest.json').read_text(encoding='utf-8-sig'))
 pack=(source/'hd-terrain-pilot.kfht').read_bytes()
 magic,version,count,reserved=struct.unpack_from('<4I',pack)
 if (magic,version,count,reserved)!=(0x5448464b,2,601,0):raise ValueError('Expected the accepted complete 601-tile pack')
 if digest(pack)!=m['package']['sha256']:raise ValueError('Pack hash does not match the accepted manifest')
 (destination/'tiles').mkdir(parents=True,exist_ok=True)
 entries={c['tile']:c for c in m['candidates']};at=16;payload=bytearray();records=[]
 for n in range(count):
  tile,size,res=struct.unpack_from('<3I',pack,at);at+=12
  native=pack[at:at+1024];at+=1024;pixels=pack[at:at+size*size];at+=size*size
  if size!=128 or res or len(pixels)!=16384:raise ValueError('Invalid accepted tile')
  c=entries[tile];png=(source/c['indexed_file']).read_bytes()
  with Image.open(source/c['indexed_file']) as image:
   if image.mode!='P' or image.tobytes()!=pixels:raise ValueError('PNG and runtime pixels differ')
  name=f'tiles/{tile:04d}.png'
  target=destination/name
  if target.exists() and target.read_bytes()!=png:raise ValueError(f'Refusing to replace different published pixels: {target}')
  target.write_bytes(png)
  records.append({'tile':tile,'dimension':size,'offset':len(payload),'bytes':len(pixels),'sha256':digest(pixels),'native_sha256':digest(native),'png':name,'png_sha256':digest(png),'family':c.get('family') or c.get('label') or 'Accepted wall restoration'})
  payload.extend(pixels)
 if at!=len(pack):raise ValueError('Trailing pack data')
 (destination/'terrain.index8').write_bytes(payload)
 result={'schema':1,'id':'dungeon-keeper-standard-terrain-4x-v1','scale':4,'native_dimension':32,'replacement_dimension':128,'count':601,'payload':'terrain.index8','payload_bytes':len(payload),'payload_sha256':digest(payload),'runtime_pack_sha256':digest(pack),'native_files':{'tmapa000.dat':{'bytes':557056,'sha256':m['source_sha256']},'tmapb000.dat':{'bytes':90112,'sha256':m['source_b_sha256']}},'palette_sha256':m['palette_sha256'],'preserved_native_tiles':m['full_terrain']['retained_original'],'tiles':records}
 (destination/'manifest.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
 print(json.dumps({'enhanced_pngs':count,'indexed_bytes':len(payload),'native_pixel_bytes_exported':0,'runtime_pack_sha256':result['runtime_pack_sha256']}))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--source',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();export(a.source,a.output)
