# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic lossless reconstruction and publication-boundary regression tests."""
import io
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
import zipfile
import zlib
from PIL import Image
import import_remaster_sprites as installer
from remaster_sprite_codec import (AssetError,OriginalBank,HEADER,SCHEMA,IDENTITY,AUTHORITY,
    digest,encode_json,png_bytes,store_object,load_object,reconstruct_member,parse_json,
    safe_relative,pixel_streams,decode_pixel_stream,published_png)

class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix=".sprite-test-",dir=Path(__file__).parent)
        self.root=Path(self.tmp.name);self.assets=self.root/"assets";self.data=self.root/"game/data"
        self.data.mkdir(parents=True);self.assets.mkdir()
        palette=bytearray(768);palette[3:6]=bytes((20,30,40))
        conversion=bytearray(768);conversion[3:6]=bytes((50,10,5))
        files={"creature.tab":HEADER.pack(0,1,1,3,3,0,1,1,1,-1,-2),
            "creature.jty":bytes((1,1,0)),"palette.dat":bytes(palette),
            "png_conv_pal.dat":bytes(conversion)}
        for n,b in files.items():(self.data/n).write_bytes(b)
        self.bank=OriginalBank(self.data)
        self.fallback=self.bank.image(0,"source-rgb")
        self.recipe={"table_index":0,"header":self.bank.header(0),"mode":"source-rgb",
            "crop":[0,0,3,3],"scale":1,"canvas":[3,3],"paste":[0,0],"background":[0,0,0,0]}
        enhanced=Image.new("RGBA",(4,4),(8,8,8,255))
        metadata=b'[{"name":"TEST","pixel_encoding":"raw-index","td":[[{"file":"enhanced.png"}]],"fp":[[{"file":"fallback.png"}]]}]'
        fallback=png_bytes(self.fallback)
        self.enhanced=png_bytes(enhanced)
        self.frames=[
            {"name":"enhanced.png","sha256":digest(self.enhanced),"bytes":len(self.enhanced),"size":[4,4],"rgba_sha256":digest(enhanced.tobytes()),"source":{"kind":"enhanced","object":store_object(self.assets,self.enhanced)}},
            {"name":"fallback.png","sha256":digest(fallback),"bytes":len(fallback),"size":[3,3],"rgba_sha256":digest(self.fallback.tobytes()),"source":{"kind":"original","recipe":self.recipe,"encoder":{"compress_level":6,"optimize":False}}},
            {"name":"sprites.json","sha256":digest(metadata),"bytes":len(metadata),"source":{"kind":"metadata","object":store_object(self.assets,metadata)}}]
        recipe={"schema":SCHEMA,"archive":"fxdata/test.zip","reviewed_zip_sha256":"a"*64,"members":self.frames}
        record={"file":"fxdata/test.zip","recipe":store_object(self.assets,encode_json(recipe)),
            "reviewed_zip_sha256":"a"*64,"members":3}
        cfg=b"[sprites]\nStand=TEST\n"
        self.manifest={"schema":SCHEMA,"id":IDENTITY,"authority_manifest_sha256":AUTHORITY,
            "original_files_sha256":self.bank.hashes,"original_archives_sha256":{},
            "archives":[record],"files":[{"file":"creatrs/test.cfg","sha256":digest(cfg),"bytes":len(cfg),"object":store_object(self.assets,cfg)}]}
        (self.assets/"manifest.json").write_bytes(encode_json(self.manifest))
        self.output=self.root/"keeper_remaster_default"
        self.originals={p:p.read_bytes() for p in self.data.iterdir()}

    def tearDown(self):
        for p,raw in self.originals.items():
            self.assertEqual(p.read_bytes(),raw)
        self.tmp.cleanup()

    def test_roundtrip_exact_members_and_raw_index_metadata(self):
        result=installer.build(self.assets,self.data,self.output)
        with zipfile.ZipFile(self.output/"fxdata/test.zip") as z:
            self.assertEqual(z.namelist(),[f["name"] for f in self.frames])
            for f in self.frames:self.assertEqual(digest(z.read(f["name"])),f["sha256"])
        self.assertEqual(installer.build(self.assets,None,self.output,True),result)
        all_blobs=list(self.assets.glob("objects/*/*.z"))
        restored=[zlib.decompress(p.read_bytes()) for p in all_blobs]
        self.assertNotIn(png_bytes(self.fallback),restored)

    def test_corrupt_blob_refused_without_publishing_output(self):
        d=self.frames[0]["source"]["object"];p=self.assets/"objects"/d["sha256"][:2]/(d["sha256"]+".z")
        p.write_bytes(p.read_bytes()+b"broken")
        with self.assertRaises(AssetError):installer.build(self.assets,self.data,self.output)
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".keeper_remaster_default-*")))

    def test_wrong_authority_refused(self):
        self.manifest["authority_manifest_sha256"]="b"*64
        (self.assets/"manifest.json").write_bytes(encode_json(self.manifest))
        with self.assertRaises(AssetError):installer.build(self.assets,self.data,self.output)
        self.assertFalse(self.output.exists())

    def test_wrong_original_hash_fails_before_staging(self):
        self.manifest["original_files_sha256"]["creature.jty"]="f"*64
        (self.assets/"manifest.json").write_bytes(encode_json(self.manifest))
        with self.assertRaises(AssetError):installer.build(self.assets,self.data,self.output)
        self.assertFalse(self.output.exists())

    def test_bad_output_path_and_unrelated_directory_preserved(self):
        self.output.mkdir();sentinel=self.output/"keep.txt";sentinel.write_text("user file")
        with self.assertRaises(AssetError):installer.build(self.assets,self.data,self.output)
        self.assertEqual(sentinel.read_text(),"user file")
        self.manifest["files"][0]["file"]="../escaped.cfg"
        (self.assets/"manifest.json").write_bytes(encode_json(self.manifest))
        with self.assertRaises(AssetError):installer.load_manifest(self.assets)
        self.assertFalse((self.root/"escaped.cfg").exists())

    def test_receipt_forgery_does_not_bypass_member_check(self):
        installer.build(self.assets,self.data,self.output)
        path=self.output/"fxdata/test.zip"
        with zipfile.ZipFile(path,"w") as z:
            for f in self.frames:z.writestr(f["name"],b"wrong" if f["name"]=="enhanced.png" else reconstruct_member(self.assets,f,self.bank))
        receipt=json.loads((self.output/installer.RECEIPT).read_text())
        receipt["files"]["fxdata/test.zip"]=digest(path.read_bytes())
        (self.output/installer.RECEIPT).write_bytes(encode_json(receipt))
        with self.assertRaises(AssetError):installer.build(self.assets,None,self.output,True)

    def test_original_archive_reconstruction_and_hash_guard(self):
        directory=self.data.parent/"fxdata";directory.mkdir()
        original=png_bytes(Image.new("RGBA",(2,2),(9,10,11,255)))
        path=directory/"creatures.zip"
        with zipfile.ZipFile(path,"w") as z:z.writestr("orc/eat.png",original)
        description={"archive":"fxdata/creatures.zip","archive_sha256":digest(path.read_bytes()),
            "member":"orc/eat.png","member_sha256":digest(original),"crop":[0,0,2,2],
            "scale":1,"canvas":[2,2],"paste":[0,0],"background":[0,0,0,0]}
        self.assertEqual(self.bank.reconstruct(description).tobytes(),Image.open(io.BytesIO(original)).tobytes())
        description["member_sha256"]="0"*64
        with self.assertRaises(AssetError):self.bank.reconstruct(description)

    def test_padded_nn_original_fallback_is_exact(self):
        description=dict(self.recipe,crop=[1,1,2,2],scale=4,canvas=[12,12],paste=[4,4])
        image=self.bank.reconstruct(description)
        self.assertEqual(image.crop((4,4,8,8)).tobytes(),Image.new("RGBA",(4,4),(80,120,160,255)).tobytes())
        self.assertEqual(image.getchannel("A").getbbox(),(4,4,8,8))

    def test_conversion_palette_preserves_original_additive_index(self):
        image=self.bank.image(0,"conversion-index")
        self.assertEqual(image.getpixel((1,1)),(200,40,20,255))
        self.assertNotEqual(image.getpixel((1,1)),self.fallback.getpixel((1,1)))

    def test_indexed_stream_preserves_hidden_RGB_and_alpha(self):
        for soft in (False,True):
            pixels=bytes(v for n in range(91) for v in
                ((n%7)*20,(n%3)*40,(n%5)*30,(n*17)%256 if soft else (255 if n%3 else 0)))
            image=Image.frombytes("RGBA",(13,7),pixels)
            stream=next(pixel_streams(image));self.assertTrue(stream.startswith(b"KFXPX1"))
            restored=decode_pixel_stream(stream)
            self.assertEqual(restored.tobytes(),pixels)
            self.assertEqual(png_bytes(restored),png_bytes(image))

    def test_RGBA_stream_retains_more_than256_RGB_colours(self):
        pixels=bytes(v for n in range(400) for v in (n%256,n//256,(n*17)%256,(n*11)%256))
        image=Image.frombytes("RGBA",(20,20),pixels)
        stream=next(pixel_streams(image));self.assertTrue(stream.startswith(b"KFXRGBA1"))
        self.assertEqual(decode_pixel_stream(stream).tobytes(),pixels)
        with self.assertRaises(AssetError):decode_pixel_stream(stream[:-1])

    def test_pixel_member_still_requires_exact_PNG_bytes(self):
        image=Image.open(io.BytesIO(self.enhanced)).convert("RGBA")
        frame=dict(self.frames[0],source={"kind":"enhanced","pixels":store_object(self.assets,next(pixel_streams(image))),"encoder":{"compress_level":9,"optimize":False}})
        with self.assertRaises(AssetError):reconstruct_member(self.assets,frame,self.bank)

    def test_published_PNG_requires_allowlisted_path_and_hash(self):
        assets=self.root/"repo/assets/remaster-sprites";assets.mkdir(parents=True)
        name="assets/creatures/frames/test.png";path=self.root/"repo"/name;path.parent.mkdir(parents=True);path.write_bytes(self.enhanced)
        description={"file":name,"sha256":digest(self.enhanced),"bytes":len(self.enhanced)}
        allow={name:{"sha256":digest(self.enhanced),"bytes":len(self.enhanced)}}
        self.assertEqual(published_png(assets,description,allow),self.enhanced)
        with self.assertRaises(AssetError):published_png(assets,description,{})
        with self.assertRaises(AssetError):published_png(assets,dict(description,file="../outside.png"),allow)
        path.write_bytes(b"bad")
        with self.assertRaises(AssetError):published_png(assets,description,allow)

    def test_compressed_size_bomb_and_duplicate_json_refused(self):
        desc=store_object(self.assets,b"a"*1024);desc["bytes"]=1
        with self.assertRaises(AssetError):load_object(self.assets,desc)
        with self.assertRaises(AssetError):parse_json(b'{"x":1,"x":2}')
        for name in ("../escape","C:/escape","fxdata/CON.zip","fxdata/a/../b"):
            with self.assertRaises(AssetError):safe_relative(name)

if __name__=="__main__":
    unittest.main(verbosity=2)
