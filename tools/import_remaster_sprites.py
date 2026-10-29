# SPDX-License-Identifier: GPL-2.0-or-later
"""Reconstruct reviewed sprite ZIPs from enhanced objects and player-owned originals.

Requires Pillow. Writes only a new output directory; never changes game settings
or mod selection. Every ZIP member must restore byte-for-byte.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import tempfile
import zipfile
from remaster_sprite_codec import (AssetError, SCHEMA, PUBLICATION_IDENTITIES, MAX_OBJECT, digest, encode_json,
    parse_json, safe_relative, no_links, read_regular, load_object, OriginalBank,
    reconstruct_member, rename_new_directory)

RECEIPT="install-receipt.json"
KIND="keeperfx-remaster-sprite-install"

def load_manifest(assets):
    raw=read_regular(assets/"manifest.json")
    m=parse_json(raw,"manifest.json")
    if (m.get("schema")!=SCHEMA or m.get("id") not in PUBLICATION_IDENTITIES or
            m.get("authority_manifest_sha256")!=PUBLICATION_IDENTITIES[m["id"]]):
        raise AssetError("Unsupported sprite publication identity/schema")
    if not isinstance(m.get("archives"),list) or not m["archives"] or not isinstance(m.get("files"),list):
        raise AssetError("Manifest needs archives and files")
    names=[]
    for p in [*m["archives"],*m["files"]]:
        name=p["file"];safe_relative(name)
        if name.casefold()==RECEIPT or not name.startswith(("fxdata/","creatrs/")):
            raise AssetError("Invalid sprite output path")
        names.append(name.casefold())
    if len(set(names))!=len(names):
        raise AssetError("Duplicate/case-colliding output")
    return m,digest(raw)

def load_recipe(assets,record):
    recipe=parse_json(load_object(assets,record["recipe"]),"archive recipe")
    if (recipe.get("schema"),recipe.get("archive"),recipe.get("reviewed_zip_sha256")) != (SCHEMA,record["file"],record["reviewed_zip_sha256"]):
        raise AssetError("Recipe identity differs")
    members=recipe.get("members")
    if not isinstance(members,list) or len(members)!=record["members"]:
        raise AssetError("Recipe member count differs")
    names=[]
    for m in members:
        safe_relative(m["name"]);names.append(m["name"].casefold())
        if type(m["bytes"]) is not int or not 0<=m["bytes"]<=MAX_OBJECT:
            raise AssetError("Invalid member size")
        if m["source"]["kind"]=="metadata" and m["name"]!="sprites.json":
            raise AssetError("Unexpected metadata archive member")
        if m["name"]!="sprites.json" and not m["name"].endswith(".png"):
            raise AssetError("Unexpected archive member")
    if len(set(names))!=len(names) or names.count("sprites.json")!=1:
        raise AssetError("Duplicate or missing archive member")
    return recipe

def verify_archive(path,recipe):
    with zipfile.ZipFile(path) as archive:
        if archive.namelist()!=[m["name"] for m in recipe["members"]]:
            raise AssetError("Reconstructed ZIP members/order differ")
        for frame in recipe["members"]:
            if archive.getinfo(frame["name"]).file_size!=frame["bytes"]:
                raise AssetError("Reconstructed ZIP member size differs")
            raw=archive.read(frame["name"])
            if len(raw)!=frame["bytes"] or digest(raw)!=frame["sha256"]:
                raise AssetError("Reconstructed ZIP member differs")

def output_names(manifest):
    return {p["file"] for p in [*manifest["archives"],*manifest["files"]]}

def verify_output(assets,output,manifest,manifest_hash):
    receipt=parse_json(read_regular(output/RECEIPT),RECEIPT)
    if (receipt.get("schema"),receipt.get("id"),receipt.get("kind"),receipt.get("manifest_sha256"),
            receipt.get("authority_manifest_sha256")) != (SCHEMA,manifest["id"],KIND,manifest_hash,
                                                         manifest["authority_manifest_sha256"]):
        raise AssetError("Output receipt differs from the selected assets")
    expected=output_names(manifest)
    if not isinstance(receipt.get("files"),dict) or set(receipt["files"])!=expected:
        raise AssetError("Unexpected receipt file list")
    actual=set();dirs=set()
    for directory,children,names in os.walk(output,followlinks=False):
        for name in [*children,*names]:
            no_links(Path(directory)/name)
        dirs.update((Path(directory)/n).relative_to(output).as_posix() for n in children)
        actual.update((Path(directory)/n).relative_to(output).as_posix() for n in names)
    expected_dirs={p.rsplit("/",1)[0] for p in expected}
    if actual!=expected|{RECEIPT} or dirs!=expected_dirs:
        raise AssetError("Output has unexpected/missing files or directories")
    for name,sha in receipt["files"].items():
        if digest(read_regular(output/safe_relative(name)))!=sha:
            raise AssetError("Installed file hash mismatch")
    for record in manifest["archives"]:
        verify_archive(output/safe_relative(record["file"]),load_recipe(assets,record))
    for record in manifest["files"]:
        raw=read_regular(output/safe_relative(record["file"]))
        if len(raw)!=record["bytes"] or digest(raw)!=record["sha256"]:
            raise AssetError("Installed configuration differs from the manifest")
    return receipt

def build(assets:Path,data:Path|None,output:Path,verify_only=False):
    assets=Path(os.path.abspath(assets));output=Path(os.path.abspath(output))
    no_links(assets);no_links(output)
    manifest,manifest_hash=load_manifest(assets)
    if output==assets or output.is_relative_to(assets) or assets.is_relative_to(output):
        raise AssetError("Output must be separate from publication assets")
    if verify_only or output.exists():
        if not output.is_dir():
            raise AssetError("Output must be an existing verified directory")
        return verify_output(assets,output,manifest,manifest_hash)
    if data is None:
        raise AssetError("--data is required when reconstructing")
    data=Path(data).resolve(strict=True)
    if output==data or output.is_relative_to(data) or data.is_relative_to(output):
        raise AssetError("Output must be separate from original data")
    bank=OriginalBank(data,manifest["original_files_sha256"],manifest["original_archives_sha256"])
    no_links(output.parent);output.parent.mkdir(parents=True,exist_ok=True)
    stage=Path(tempfile.mkdtemp(prefix="."+output.name+"-",dir=output.parent))
    try:
        files={}
        for record in manifest["archives"]:
            recipe=load_recipe(assets,record)
            path=stage/safe_relative(record["file"]);path.parent.mkdir(parents=True,exist_ok=True)
            with zipfile.ZipFile(path,"w",compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
                for frame in recipe["members"]:
                    raw=reconstruct_member(assets,frame,bank,manifest.get("published_pngs",{}))
                    info=zipfile.ZipInfo(frame["name"],(1980,1,1,0,0,0))
                    info.create_system=3;info.external_attr=0o100644<<16
                    archive.writestr(info,raw,compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
            verify_archive(path,recipe);files[record["file"]]=digest(path.read_bytes())
        for record in manifest["files"]:
            raw=load_object(assets,record["object"])
            if len(raw)!=record["bytes"] or digest(raw)!=record["sha256"]:
                raise AssetError("Config object differs")
            path=stage/safe_relative(record["file"]);path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(raw);files[record["file"]]=digest(raw)
        receipt={"schema":SCHEMA,"id":manifest["id"],"kind":KIND,"manifest_sha256":manifest_hash,
            "authority_manifest_sha256":manifest["authority_manifest_sha256"],"files":files,
            "fallback_source_sha256":bank.hashes,"fallback_original_archives_sha256":bank.archive_hashes}
        (stage/RECEIPT).write_bytes(encode_json(receipt))
        verify_output(assets,stage,manifest,manifest_hash)
        no_links(output)
        if output.exists():
            raise AssetError("Output appeared while reconstructing")
        rename_new_directory(stage,output);stage=None
        return receipt
    finally:
        if stage is not None:
            no_links(stage)
            if stage.resolve().parent!=output.parent.resolve() or not stage.name.startswith("."+output.name+"-"):
                raise AssetError("Refusing cleanup outside owned staging directory")
            shutil.rmtree(stage)

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--assets",type=Path,default=Path(__file__).resolve().parents[1]/"assets/remaster-sprites")
    p.add_argument("--data",type=Path);p.add_argument("--output",type=Path,required=True)
    p.add_argument("--verify-only",action="store_true");a=p.parse_args(argv)
    try:
        receipt=build(a.assets,a.data,a.output,a.verify_only)
    except (AssetError,OSError,KeyError,TypeError,zipfile.BadZipFile) as error:
        p.exit(1,f"Remaster sprites: {error}\n")
    print(encode_json({"verified":True,"output":str(a.output),"manifest_sha256":receipt["manifest_sha256"],"files":len(receipt["files"])}).decode().strip())

if __name__=="__main__":
    main()
