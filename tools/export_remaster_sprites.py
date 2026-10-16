# SPDX-License-Identifier: GPL-2.0-or-later
"""Export selected sprite archives as deduplicated lossless objects and original recipes."""
from __future__ import annotations
import argparse
import collections
import io
import json
from pathlib import Path
import sys
import zlib
import zipfile
import PIL
from PIL import Image
from remaster_sprite_codec import (AssetError, OriginalBank, SCHEMA, IDENTITY, digest,
    encode_json, parse_json, safe_relative, read_regular, write_checked, store_object,
    rgba_image, png_bytes, pixel_key, SOURCE_NAMES)

AUTHORITY = "dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf"
MODES = ("source-rgb","conversion-index","quantized-source","raw-index")

def encoder_for(image, expected):
    for optimize, levels in ((False,(6,9,1,0,2,3,4,5,7,8)),(True,(6,))):
        for level in levels:
            options={"compress_level":level,"optimize":optimize}
            if png_bytes(image,options)==expected:
                return options
    raise AssetError("Original/transparent PNG is not reproducible by the supported encoder")

class OriginalIndex:
    def __init__(self, bank):
        self.bank=bank;self.full={};self.cropped={};self.valid=0;self.skipped=[];self.custom_sources={}
        for index in range(bank.count):
            try:
                bank.indices(index)
            except AssetError:
                self.skipped.append(index);continue
            self.valid+=1
            for mode in MODES:
                image=bank.image(index,mode)
                self.full.setdefault(pixel_key(image),(index,mode,[0,0,*image.size]))
                box=image.getchannel("A").getbbox()
                if box:
                    self.cropped.setdefault(pixel_key(image.crop(box)),(index,mode,list(box)))
            if index and index%1500==0:
                print(f"Indexed {index}/{bank.count} original records",flush=True)

        # Upstream custom originals live beside data/ in the base fxdata bank.
        for path in sorted((bank.data.parent/"fxdata").glob("*.zip")):
            relative="fxdata/"+path.name
            raw=read_regular(path);archive_sha=digest(raw)
            with zipfile.ZipFile(io.BytesIO(raw)) as archive:
                for name in archive.namelist():
                    if not name.endswith(".png"):
                        continue
                    safe_relative(name)
                    png=archive.read(name);image=rgba_image(png)
                    key=relative+"::"+name
                    self.custom_sources[key]={"archive":relative,"archive_sha256":archive_sha,
                        "member":name,"member_sha256":digest(png)}
                    self.full.setdefault(pixel_key(image),(key,"archive-rgba",[0,0,*image.size]))
                    box=image.getchannel("A").getbbox()
                    if box:
                        self.cropped.setdefault(pixel_key(image.crop(box)),(key,"archive-rgba",list(box)))
            print("Indexed original archive "+relative,flush=True)

    def match(self, image):
        match=self.full.get(pixel_key(image))
        if match:
            index,mode,crop=match
            return self.recipe(index,mode,crop,1,image.size,(0,0),(0,0,0,0),image)
        box=image.getchannel("A").getbbox()
        if not box:
            return None
        cut=image.crop(box)
        # Only exact integer pixel replication and transparent padding is allowed.
        for factor in (1,2,4,8):
            if cut.width%factor or cut.height%factor:
                continue
            small=cut.resize((cut.width//factor,cut.height//factor),Image.Resampling.NEAREST)
            if small.resize(cut.size,Image.Resampling.NEAREST).tobytes()!=cut.tobytes():
                continue
            match=self.cropped.get(pixel_key(small))
            if match:
                index,mode,crop=match
                result=self.recipe(index,mode,crop,factor,image.size,box[:2],image.getpixel((0,0)),image)
                if result:
                    return result
        return None

    def recipe(self,index,mode,crop,factor,canvas,xy,background,expected):
        if background[3]!=0:
            background=(0,0,0,0)
        source=({"table_index":index,"header":self.bank.header(index),"mode":mode}
            if type(index) is int else self.custom_sources[index])
        recipe={**source,"crop":list(crop),"scale":factor,"canvas":list(canvas),
            "paste":list(xy),"background":list(background)}
        if self.bank.reconstruct(recipe).tobytes()!=expected.tobytes():
            return None
        return recipe

def export(selection,data,destination):
    import zipfile
    raw=read_regular(selection);selected=parse_json(raw,str(selection))
    if selected.get("manifest_sha256")!=AUTHORITY:
        raise AssetError("Selection differs from the reviewed V13 appearance authority")
    source=Path(selected["root"])
    packages=[f for f in selected["files"] if f["file"].endswith(".zip")]
    if len(packages)!=41:
        raise AssetError("Expected all41 selected custom sprite archives")
    bank=OriginalBank(data);index=OriginalIndex(bank)
    counts=collections.Counter();records=[];used_archives={};local={"schema":1,"selection_file_sha256":digest(raw),
        "authority_manifest_sha256":AUTHORITY,"archives":[],"source_index_valid":index.valid,
        "source_index_skipped":index.skipped,"unreconstructable":[]}
    for package in packages:
        relative=package["file"];archive_raw=read_regular(source/safe_relative(relative))
        if digest(archive_raw)!=package["sha256"]:
            raise AssetError(f"Selected archive hash differs: {relative}")
        archive=zipfile.ZipFile(io.BytesIO(archive_raw))
        if archive.testzip() is not None or len(set(n.casefold() for n in archive.namelist()))!=len(archive.namelist()):
            raise AssetError("Bad or duplicate source ZIP member")
        sprite_raw=archive.read("sprites.json");sprites=parse_json(sprite_raw,"sprites.json")
        contexts=collections.defaultdict(list)
        for e in sprites:
            for view in ("td","fp"):
                scale=e.get(view+"_resolution_scale",e.get("resolution_scale",1))
                if scale not in (1,2,4,8):
                    raise AssetError("Unsupported view resolution scale")
                for d,seq in enumerate(e[view]):
                    for phase,f in enumerate(seq):
                        contexts[f["file"]].append((view,scale,e["name"],d,phase))
        extras=set(archive.namelist())-{"sprites.json",*contexts}
        if not set(contexts)<=set(archive.namelist()) or any(not n.endswith(".png") for n in extras):
            raise AssetError(f"Unaccounted non-PNG archive members: {relative}")
        recipe={"schema":SCHEMA,"archive":relative,"reviewed_zip_sha256":package["sha256"],
            "members":[]}
        tally=collections.Counter()
        for name in archive.namelist():
            safe_relative(name);member=archive.read(name)
            frame={"name":name,"sha256":digest(member),"bytes":len(member),"referenced":name=="sprites.json" or name in contexts}
            if name=="sprites.json":
                frame["source"]={"kind":"metadata","object":store_object(destination,member)}
            else:
                image=rgba_image(member)
                frame.update(size=list(image.size),rgba_sha256=digest(image.tobytes()))
                native=index.match(image)
                if native is not None:
                    if "archive" in native:
                        used_archives[native["archive"]]=native["archive_sha256"]
                    frame["source"]={"kind":"original","recipe":native,
                        "encoder":encoder_for(image,member)}
                elif not image.getchannel("A").getbbox() and len(image.getcolors(maxcolors=2) or [])==1:
                    frame["source"]={"kind":"transparent","canvas":list(image.size),
                        "rgba":list(image.getpixel((0,0))),"encoder":encoder_for(image,member)}
                else:
                    lowres_fp=any(v=="fp" and s==1 for v,s,*_ in contexts[name])
                    known_fallback=(relative=="fxdata/dark_mistress-sprites.zip" and name=="slap/td/d2-01.png")
                    if lowres_fp or known_fallback or name.startswith("references/"):
                        local["unreconstructable"].append({"archive":relative,"member":name,"reason":"Known original fallback did not match original reconstruction"})
                        raise AssetError(f"Original fallback cannot safely reconstruct: {relative}:{name}")
                    frame["source"]={"kind":"enhanced","object":store_object(destination,member)}
            kind=frame["source"]["kind"];tally[kind]+=1;counts[kind]+=1
            recipe["members"].append(frame)
        recipe_blob=store_object(destination,encode_json(recipe))
        records.append({"file":relative,"recipe":recipe_blob,"reviewed_zip_sha256":package["sha256"],
            "members":len(recipe["members"]),"counts":dict(tally)})
        local["archives"].append({"file":relative,"reviewed_zip_sha256":package["sha256"],
            "counts":dict(tally),"members":[{"name":m["name"],"sha256":m["sha256"],
            "source":m["source"]["kind"],"rgba_sha256":m.get("rgba_sha256")} for m in recipe["members"]]})
        print(f"Exported {relative}: {dict(tally)}",flush=True)
    files=[]
    for record in selected["files"]:
        if not record["file"].endswith(".cfg"):
            continue
        content=read_regular(source/safe_relative(record["file"]))
        if digest(content)!=record["sha256"]:
            raise AssetError("Selected configuration hash differs")
        files.append({"file":record["file"],"sha256":digest(content),"bytes":len(content),
            "object":store_object(destination,content)})
    manifest={"schema":SCHEMA,"id":IDENTITY,"authority_manifest_sha256":AUTHORITY,
        "original_files_sha256":bank.hashes,"original_archives_sha256":used_archives,"archives":records,"files":files,
        "counts":dict(counts),"png_encoder":{"pillow":PIL.__version__,"zlib":zlib.ZLIB_RUNTIME_VERSION,
        "contract":"Fallback PNG member bytes must match; fail closed on encoder differences."},
        "notice":"NOTICE.md","output_mod_name":"keeper_remaster_default"}
    write_checked(destination/"manifest.json",encode_json(manifest))
    local["manifest_sha256"]=digest(encode_json(manifest));local["counts"]=dict(counts)
    local["excluded_selection_files"]=[f["file"] for f in selected["files"] if not f["file"].endswith((".zip",".cfg"))]
    local["excluded_native_frontend_resources"]=len(selected.get("native_overlays",[]))
    local["original_pixels_published"]=0
    return manifest,local

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--selection",type=Path,required=True);p.add_argument("--data",type=Path,required=True)
    p.add_argument("--destination",type=Path,required=True);p.add_argument("--report",type=Path,required=True)
    a=p.parse_args(argv)
    try:
        manifest,report=export(a.selection.absolute(),a.data.absolute(),a.destination.absolute())
        write_checked(a.report.absolute(),encode_json(report))
    except (AssetError,OSError,KeyError,TypeError) as error:
        p.exit(1,f"Sprite export: {error}\n")
    print(json.dumps({"archives":len(manifest["archives"]),"counts":manifest["counts"],
        "manifest_sha256":digest(encode_json(manifest))},indent=2))

if __name__=="__main__":
    main()
