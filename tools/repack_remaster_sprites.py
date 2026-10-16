# SPDX-License-Identifier: GPL-2.0-or-later
"""Compact an exact sprite publication using lossless pixels and published PNG reuse."""
from __future__ import annotations
import argparse
import collections
import io
from pathlib import Path
import zlib
from PIL import Image
from remaster_sprite_codec import (AssetError,AUTHORITY,digest,encode_json,parse_json,
    read_regular,write_checked,safe_relative,load_object,store_object,rgba_image,
    png_bytes,pixel_streams,decode_pixel_stream)

def repack(source,legacy_root,destination):
    original=parse_json(read_regular(source/"manifest.json"))
    if original.get("authority_manifest_sha256")!=AUTHORITY:
        raise AssetError("Not the reviewed V13 publication")
    legacy={}
    for p in sorted((legacy_root/"assets/creatures").rglob("*.png")):
        raw=read_regular(p);legacy.setdefault(digest(raw),(p.relative_to(legacy_root).as_posix(),raw))
    manifest=dict(original);manifest["archives"]=[];manifest["files"]=[]
    published={};processed={};counts=collections.Counter();original_png_bytes=0;packed_before=0;packed_after=0
    def transfer(description):
        raw=load_object(source,description)
        new=store_object(destination,raw)
        if new!=description:
            raise AssetError("Object transfer is not deterministic with this zlib")
        return new
    for record in original["archives"]:
        recipe=parse_json(load_object(source,record["recipe"]))
        for frame in recipe["members"]:
            src=frame["source"]
            if src["kind"]=="enhanced":
                old=src["object"];sha=old["sha256"]
                if sha not in processed:
                    png=load_object(source,old);original_png_bytes+=len(png);packed_before+=old["packed_bytes"]
                    if sha in legacy:
                        name,raw=legacy[sha]
                        description={"file":name,"sha256":sha,"bytes":len(raw)}
                        published[name]={"sha256":sha,"bytes":len(raw)}
                        write_checked(destination.parents[1]/safe_relative(name),raw)
                        result={"kind":"enhanced","published":description}
                        counts["published_reuse"]+=1
                    else:
                        image=rgba_image(png);options={"compress_level":6,"optimize":False}
                        if png_bytes(image,options)!=png:
                            result={"kind":"enhanced","object":transfer(old)}
                            counts["PNG_byte_fallback"]+=1;packed_after+=old["packed_bytes"]
                        else:
                            # The stratified study found indexed streams best in106/106.
                            # The first candidate is indexed when colours<=256, RGBA otherwise.
                            pixels=next(pixel_streams(image));restored=decode_pixel_stream(pixels)
                            if restored.tobytes()!=image.tobytes() or png_bytes(restored,options)!=png:
                                raise AssetError("Lossless pixel reconstruction differs")
                            packed=zlib.compress(pixels,9)
                            if len(packed)<old["packed_bytes"]:
                                description=store_object(destination,pixels)
                                result={"kind":"enhanced","pixels":description,"encoder":options}
                                counts["indexed_pixels" if pixels.startswith(b"KFXPX1") else "RGBA_pixels"]+=1
                                packed_after+=description["packed_bytes"]
                            else:
                                result={"kind":"enhanced","object":transfer(old)}
                                counts["PNG_smaller"]+=1;packed_after+=old["packed_bytes"]
                    processed[sha]=result
                frame["source"]=processed[sha]
            elif src["kind"]=="metadata":
                frame["source"]={"kind":"metadata","object":transfer(src["object"])}
        new=dict(record);new["recipe"]=store_object(destination,encode_json(recipe));manifest["archives"].append(new)
        print("Compacted "+record["file"],flush=True)
    for record in original["files"]:
        new=dict(record);new["object"]=transfer(record["object"]);manifest["files"].append(new)
    manifest["published_pngs"]=published
    manifest["storage"]="lossless-indexed-or-RGBA-streams-with-exact-PNG-restoration-v1"
    manifest["source_publication_manifest_sha256"]=digest(read_regular(source/"manifest.json"))
    write_checked(destination/"manifest.json",encode_json(manifest))
    return manifest,{"schema":1,"source_manifest_sha256":digest(read_regular(source/"manifest.json")),
        "compact_manifest_sha256":digest(encode_json(manifest)),"unique_enhanced_pngs":len(processed),
        "counts":dict(counts),"unique_original_PNG_bytes":original_png_bytes,
        "old_enhanced_object_zlib_bytes":packed_before,"new_enhanced_object_zlib_bytes":packed_after,
        "published_reuse_bytes_already_in_repository":sum(x["bytes"] for x in published.values()),
        "accepted_PNG_mutations":0,"lossy_operations":0,"encoder_byte_mismatches_preserved_as_PNG":counts["PNG_byte_fallback"]}

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source",type=Path,required=True);p.add_argument("--legacy-root",type=Path,required=True)
    p.add_argument("--destination",type=Path,required=True);p.add_argument("--report",type=Path,required=True)
    a=p.parse_args(argv)
    try:
        manifest,report=repack(a.source.absolute(),a.legacy_root.absolute(),a.destination.absolute())
        write_checked(a.report.absolute(),encode_json(report))
    except (AssetError,OSError,KeyError,TypeError) as error:
        p.exit(1,f"Compact sprites: {error}\n")
    print(encode_json(report).decode().strip())

if __name__=="__main__":
    main()
