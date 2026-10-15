# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows installer checks with synthetic game data; no commercial fixtures."""
import gzip,hashlib,json,os,struct,subprocess,tempfile,unittest,zlib
from pathlib import Path
SCRIPT=Path(__file__).with_name('install_enhanced_terrain.ps1')
def sha(data):return hashlib.sha256(data).hexdigest()
def png(pixels):
 def chunk(name,body):return struct.pack('>I',len(body))+name+body+struct.pack('>I',zlib.crc32(name+body)&0xffffffff)
 palette=bytes(c for i in range(256) for c in (i,i,i))
 rows=b''.join(b'\0'+pixels[y*128:(y+1)*128] for y in range(128))
 return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',128,128,8,3,0,0,0))+chunk(b'PLTE',palette)+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')
@unittest.skipUnless(os.name=='nt','The supported installer runs in Windows PowerShell')
class Installer(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='keeperfx terrain test ');self.root=Path(self.temp.name);self.runtime=self.root/'runtime';self.assets=self.root/'assets'
  for p in ['fxdata','mods','data']:(self.runtime/p).mkdir(parents=True)
  (self.assets/'tiles').mkdir(parents=True)
  self.order=b'[after_base]\ncustom_audio\n[after_map]\nenhanced_creatures\nkeeper_enhanced_default\ncustom_map\n'
  (self.runtime/'mods/load_order.cfg').write_bytes(self.order);(self.runtime/'save').mkdir();(self.runtime/'save/settings.toml').write_bytes(b'personal settings')
  banks={'tmapa000.dat':b'\x07'*(544*1024),'tmapb000.dat':b'\x08'*(88*1024)};pal=bytes(i%64 for i in range(768))
  for name,data in banks.items():(self.runtime/'data'/name).write_bytes(data)
  (self.runtime/'data/palette.dat').write_bytes(pal);payload=bytearray();records=[];pack=bytearray(struct.pack('<4I',0x5448464b,2,2,0))
  for tile,index in [(2,7),(1001,8)]:
   pixels=bytes([index])*16384;image=png(pixels);name=f'tiles/{tile:04d}.png';(self.assets/name).write_bytes(image)
   records.append(dict(tile=tile,dimension=128,offset=len(payload),bytes=len(pixels),sha256=sha(pixels),native_sha256=sha(bytes([index])*1024),png=name,png_sha256=sha(image)))
   payload.extend(pixels);pack.extend(struct.pack('<3I',tile,128,0)+bytes([index])*1024+pixels)
  self.expected=bytes(pack);self.payload=bytes(payload);encoded=gzip.compress(payload,mtime=0);(self.assets/'terrain.index8.gz').write_bytes(encoded)
  self.manifest=dict(schema=2,id='dungeon-keeper-standard-terrain-4x-v1',count=2,payload='terrain.index8.gz',payload_compression='gzip',compressed_bytes=len(encoded),compressed_sha256=sha(encoded),payload_bytes=len(payload),payload_sha256=sha(payload),runtime_pack_sha256=sha(pack),palette_sha256=sha(pal),native_files={name:dict(bytes=len(data),sha256=sha(data)) for name,data in banks.items()},tiles=records);self.save_manifest()
 def tearDown(self):self.temp.cleanup()
 def save_manifest(self):(self.assets/'manifest.json').write_text(json.dumps(self.manifest),encoding='utf-8')
 def run_installer(self,*flags,success=True):
  r=subprocess.run(['powershell.exe','-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',str(SCRIPT),'-RuntimePath',str(self.runtime),'-AssetsPath',str(self.assets),*flags],capture_output=True,text=True)
  self.assertEqual(r.returncode==0,success,r.stdout+'\n'+r.stderr);return r
 def test_round_trip_preservation_and_idempotence(self):
  self.run_installer();target=self.runtime/'fxdata/standard-hd.kfht';self.assertEqual(target.read_bytes(),self.expected)
  order=(self.runtime/'mods/load_order.cfg').read_bytes();self.assertIn(b'enhanced_creatures',order);self.assertIn(b'custom_map',order);self.assertIn(b'keeper_enhanced_default disabled',order)
  self.assertEqual((self.runtime/'save/settings.toml').read_bytes(),b'personal settings')
  receipt=self.runtime/'fxdata/standard-hd-install.json';old=receipt.read_bytes();backups=sorted(self.runtime.rglob('*.bak'))
  self.run_installer();self.run_installer('-VerifyOnly');self.assertEqual(receipt.read_bytes(),old);self.assertEqual(sorted(self.runtime.rglob('*.bak')),backups)
 def test_payload_corruption_preserves_install(self):
  self.run_installer();(self.assets/'terrain.index8.gz').write_bytes(b'bad');self.run_installer(success=False)
  self.assertEqual((self.runtime/'fxdata/standard-hd.kfht').read_bytes(),self.expected)
 def replace_archive(self,encoded):
  (self.assets/'terrain.index8.gz').write_bytes(encoded)
  self.manifest.update(compressed_bytes=len(encoded),compressed_sha256=sha(encoded));self.save_manifest()
 def assert_no_install(self):
  self.run_installer(success=False)
  self.assertFalse((self.runtime/'fxdata/standard-hd.kfht').exists());self.assertEqual((self.runtime/'mods/load_order.cfg').read_bytes(),self.order)
 def test_invalid_gzip_with_matching_archive_hash_is_rejected(self):
  self.replace_archive(b'not a gzip archive');self.assert_no_install()
 def test_short_decoded_payload_is_rejected(self):
  self.replace_archive(gzip.compress(self.payload[:-1],mtime=0));self.assert_no_install()
 def test_excess_decoded_payload_is_rejected(self):
  self.replace_archive(gzip.compress(self.payload+b'x',mtime=0));self.assert_no_install()
 def test_decoded_payload_hash_mismatch_is_rejected(self):
  self.replace_archive(gzip.compress(b'x'+self.payload[1:],mtime=0));self.assert_no_install()
 def test_native_mismatch_has_no_mutations(self):
  p=self.runtime/'data/tmapa000.dat';p.write_bytes(b'x'+p.read_bytes()[1:]);self.run_installer(success=False)
  self.assertFalse((self.runtime/'fxdata/standard-hd.kfht').exists());self.assertEqual((self.runtime/'mods/load_order.cfg').read_bytes(),self.order)
 def test_png_corruption_is_rejected(self):
  (self.assets/'tiles/0002.png').write_bytes(b'bad');self.run_installer(success=False);self.assertFalse((self.runtime/'fxdata/standard-hd.kfht').exists())
 def test_duplicate_records_are_rejected(self):
  self.manifest['tiles'][1]['tile']=2;self.save_manifest();self.run_installer(success=False);self.assertFalse((self.runtime/'fxdata/standard-hd.kfht').exists())
 def test_modified_installed_pack_is_preserved(self):
  self.run_installer();p=self.runtime/'fxdata/standard-hd.kfht';p.write_bytes(b'modified');self.run_installer(success=False);self.assertEqual(p.read_bytes(),b'modified')
 def test_unrecognized_receipt_is_preserved(self):
  p=self.runtime/'fxdata/standard-hd-install.json';p.write_bytes(b'personal receipt');self.run_installer(success=False);self.assertEqual(p.read_bytes(),b'personal receipt');self.assertFalse((self.runtime/'fxdata/standard-hd.kfht').exists())
 def test_unrecognized_target_is_preserved(self):
  p=self.runtime/'fxdata/standard-hd.kfht';p.write_bytes(b'personal file');self.run_installer(success=False);self.assertEqual(p.read_bytes(),b'personal file');self.assertEqual((self.runtime/'mods/load_order.cfg').read_bytes(),self.order)
if __name__=='__main__':unittest.main(verbosity=2)
