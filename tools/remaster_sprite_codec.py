# SPDX-License-Identifier: GPL-2.0-or-later
"""Lossless sprite-object storage and player-owned original frame reconstruction."""
from __future__ import annotations
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import zlib
import zipfile
from functools import lru_cache
from PIL import Image

SCHEMA = 2
IDENTITY = "remaster-v13-sprites"
AUTHORITY = "dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf"
HEADER = struct.Struct("<I8Bhh")
MAX_OBJECT = 64 * 1024 * 1024
SOURCE_NAMES = ("creature.tab", "creature.jty", "palette.dat", "png_conv_pal.dat")

class AssetError(ValueError):
    """Publication input or reconstructed output does not match its contract."""

def digest(data):
    return hashlib.sha256(data).hexdigest()

def encode_json(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()

def parse_json(data, label="JSON"):
    def unique(pairs):
        result = {}
        for k, v in pairs:
            if k in result:
                raise AssetError(f"Duplicate key in {label}: {k}")
            result[k] = v
        return result
    try:
        return json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AssetError(f"Invalid {label}") from error

def safe_relative(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise AssetError(f"Unsafe relative path: {value!r}")
    parts = value.split("/")
    if PurePosixPath(value).is_absolute() or any(
        p in ("", ".", "..") or p.endswith((".", " ")) or
        any(ord(c) < 32 for c in p) or
        re.match(r"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", p, re.I)
        for p in parts
    ):
        raise AssetError(f"Unsafe relative path: {value!r}")
    return Path(*parts)

def no_links(path):
    for item in (path, *path.parents):
        if item.is_symlink() or (hasattr(item, "is_junction") and item.is_junction()):
            raise AssetError(f"Links/junctions are not permitted: {item}")

def read_regular(path):
    no_links(path)
    if not path.is_file():
        raise AssetError(f"Regular file missing: {path}")
    return path.read_bytes()

def write_checked(path, raw):
    no_links(path)
    if path.exists():
        if not path.is_file() or path.read_bytes() != raw:
            raise AssetError(f"Refusing to replace different export: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)

def object_path(root, sha):
    if not isinstance(sha, str) or not re.fullmatch("[0-9a-f]{64}", sha):
        raise AssetError("Invalid object digest")
    return root / "objects" / sha[:2] / (sha + ".z")

def store_object(root, raw):
    if len(raw) > MAX_OBJECT:
        raise AssetError("Object exceeds size limit")
    packed = zlib.compress(raw, 9)
    sha = digest(raw)
    write_checked(object_path(root, sha), packed)
    return {"sha256":sha, "bytes":len(raw), "packed_sha256":digest(packed), "packed_bytes":len(packed)}

def load_object(root, description):
    if set(description) != {"sha256", "bytes", "packed_sha256", "packed_bytes"}:
        raise AssetError("Invalid object descriptor")
    size = description["bytes"]
    if type(size) is not int or not 0 <= size <= MAX_OBJECT:
        raise AssetError("Invalid object size")
    packed_size=description["packed_bytes"]
    if type(packed_size) is not int or not 0<=packed_size<=MAX_OBJECT*2:
        raise AssetError("Invalid compressed object size")
    path=object_path(root,description["sha256"]);no_links(path)
    if path.stat().st_size!=packed_size:
        raise AssetError("Compressed object size mismatch")
    raw = read_regular(path)
    if len(raw) != packed_size or digest(raw) != description["packed_sha256"]:
        raise AssetError("Compressed object hash/size mismatch")
    try:
        decoder = zlib.decompressobj()
        plain = decoder.decompress(raw, size + 1)
    except zlib.error as error:
        raise AssetError("Invalid compressed object") from error
    if (len(plain) != size or not decoder.eof or decoder.unconsumed_tail or
            decoder.unused_data or digest(plain) != description["sha256"]):
        raise AssetError("Uncompressed object hash/size mismatch")
    return plain

def rgba_image(raw):
    try:
        with Image.open(io.BytesIO(raw)) as im:
            if im.format != "PNG" or not (0 < im.width <= 1024 and 0 < im.height <= 1024):
                raise AssetError("Expected PNG with axes in 1..1024")
            im.load()
            return im.convert("RGBA")
    except (OSError, Image.DecompressionBombError) as error:
        raise AssetError("Invalid PNG") from error

def png_bytes(image, encoder=None):
    options = encoder or {"compress_level":6, "optimize":False}
    if set(options) != {"compress_level", "optimize"} or type(options["compress_level"]) is not int or not 0 <= options["compress_level"] <= 9 or type(options["optimize"]) is not bool:
        raise AssetError("Unsupported PNG encoder options")
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", **options)
    return buffer.getvalue()

def pixel_key(image):
    return (image.size, digest(image.tobytes()))

class OriginalBank:
    """Bounded local RLE decode; includes exact original index and palette modes."""
    def __init__(self, data, expected=None, expected_archives=None):
        data = Path(data).resolve(strict=True)
        self.data = data
        self.archive_hashes = {}
        for name, sha in (expected_archives or {}).items():
            self.archive_bytes(name, sha)
        self.files = {n:read_regular(data / n) for n in SOURCE_NAMES}
        self.hashes = {n:digest(b) for n, b in self.files.items()}
        if expected is not None and self.hashes != expected:
            raise AssetError("Player original files differ from the supported source hashes")
        self.tab, self.blob = self.files["creature.tab"], self.files["creature.jty"]
        if not self.tab or len(self.tab) % HEADER.size:
            raise AssetError("Original table is not complete 16-byte records")
        for name in SOURCE_NAMES[2:]:
            if len(self.files[name]) != 768 or max(self.files[name]) > 63:
                raise AssetError("Expected original six-bit 256-colour palettes")
        self.count = len(self.tab) // HEADER.size
        self.palette = [tuple(4*v for v in self.files["palette.dat"][i:i+3]) for i in range(0,768,3)]
        self.conversion = [tuple(4*v for v in self.files["png_conv_pal.dat"][i:i+3]) for i in range(0,768,3)]
        cp = [tuple(v//4 for v in c) for c in self.conversion]
        self.quantized = [self.conversion[min(range(256), key=lambda n:sum(abs(a-b) for a,b in zip(tuple(v//4 for v in c),cp[n])))] for c in self.palette]

    def header(self, index):
        if type(index) is not int or not 0 <= index < self.count:
            raise AssetError("Original table index out of bounds")
        return list(HEADER.unpack_from(self.tab,index*HEADER.size))

    @lru_cache(maxsize=128)
    def indices(self, index):
        h = self.header(index)
        start,w,ht,fw,fh,_,_,ox,oy,_,_ = h
        if not (w and ht and fw and fh and ox+w <= fw and oy+ht <= fh and start < len(self.blob)):
            raise AssetError("Original frame has invalid geometry or byte bounds")
        pixels=bytearray(fw*fh);coverage=bytearray(fw*fh);pos=start
        for y in range(ht):
            x=0
            while True:
                if pos >= len(self.blob):
                    raise AssetError("Truncated original row")
                command=self.blob[pos];pos+=1
                if not command:
                    break
                run=command if command<128 else 256-command
                visible=min(run,w-x)
                if command<128:
                    if pos+run>len(self.blob):
                        raise AssetError("Truncated original literal")
                    at=(y+oy)*fw+ox+x
                    pixels[at:at+visible]=self.blob[pos:pos+visible]
                    coverage[at:at+visible]=bytes([255])*visible
                    pos+=run
                x+=visible
        return (fw,fh),bytes(pixels),bytes(coverage)

    def image(self, index, mode):
        size, pixels, alpha = self.indices(index)
        if mode=="source-rgb":
            palette=self.palette
        elif mode=="conversion-index":
            palette=self.conversion
        elif mode=="quantized-source":
            palette=self.quantized
        elif mode=="raw-index":
            palette=[(n,n,n) for n in range(256)]
        else:
            raise AssetError("Unsupported original colour mode")
        im=Image.frombytes("P",size,pixels)
        im.putpalette([v for c in palette for v in c])
        result=im.convert("RGBA")
        result.putalpha(Image.frombytes("L",size,alpha))
        if mode=="source-rgb":
            zero=Image.new("RGBA",size)
            zero.paste(result,(0,0),result.getchannel("A"))
            result=zero
        return result

    @lru_cache(maxsize=16)
    def archive_bytes(self, name, expected):
        relative=safe_relative(name)
        if len(relative.parts)!=2 or relative.parts[0]!="fxdata" or relative.suffix!=".zip":
            raise AssetError("Invalid original archive path")
        raw=read_regular(self.data.parent/relative)
        if digest(raw)!=expected:
            raise AssetError("Installed original archive hash differs")
        self.archive_hashes[name]=expected
        return raw

    def archive_image(self, description):
        raw=self.archive_bytes(description["archive"],description["archive_sha256"])
        safe_relative(description["member"])
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            if len(archive.namelist())!=len(set(archive.namelist())):
                raise AssetError("Duplicate original archive members")
            png=archive.read(description["member"])
        if digest(png)!=description["member_sha256"]:
            raise AssetError("Installed original archive member differs")
        return rgba_image(png)

    def reconstruct(self, description):
        if "archive" in description:
            image=self.archive_image(description)
        else:
            if self.header(description["table_index"]) != description["header"]:
                raise AssetError("Original frame header differs")
            image=self.image(description["table_index"],description["mode"])
        crop=description["crop"]
        if not isinstance(crop,list) or len(crop)!=4 or any(type(v) is not int for v in crop) or not(0<=crop[0]<crop[2]<=image.width and 0<=crop[1]<crop[3]<=image.height):
            raise AssetError("Invalid source crop")
        factor=description["scale"]
        if factor not in (1,2,4,8):
            raise AssetError("Unsupported original replication scale")
        image=image.crop(tuple(crop))
        image=image.resize((image.width*factor,image.height*factor),Image.Resampling.NEAREST)
        canvas=description["canvas"];xy=description["paste"];background=description["background"]
        if (len(canvas)!=2 or any(type(v) is not int or not 0<v<=1024 for v in canvas) or
                len(xy)!=2 or any(type(v) is not int or v<0 for v in xy) or
                xy[0]+image.width>canvas[0] or xy[1]+image.height>canvas[1] or
                len(background)!=4 or any(type(v) is not int or not 0<=v<=255 for v in background) or background[3]!=0):
            raise AssetError("Invalid original frame padding")
        result=Image.new("RGBA",tuple(canvas),tuple(background))
        result.paste(image,tuple(xy))
        return result

def pixel_streams(image):
    # Exact RGB indexing, never palette approximation. Numpy is optional for export.
    rgb=image.convert("RGB").tobytes()
    try:
        import numpy as np
        a=np.frombuffer(rgb,dtype=np.uint8).reshape(-1,3)
        codes=(a[:,0].astype(np.uint32)<<16)|(a[:,1].astype(np.uint32)<<8)|a[:,2]
        values,inverse=np.unique(codes,return_inverse=True)
        colours=[((int(c)>>16)&255,(int(c)>>8)&255,int(c)&255) for c in values]
        indices=inverse.astype(np.uint8).tobytes() if len(colours)<=256 else None
    except ImportError:
        colours=sorted(set(zip(rgb[0::3],rgb[1::3],rgb[2::3])))
        if len(colours)<=256:
            lookup={bytes(c):i for i,c in enumerate(colours)}
            indices=bytes(lookup[rgb[n:n+3]] for n in range(0,len(rgb),3))
        else:
            indices=None
    if indices is not None:
        alpha=image.getchannel("A")
        binary=set(v for count,v in (alpha.getcolors(257) or []))<={0,255}
        # More than256distinctalpha values is impossible for this8bit channel.
        alpha_bits=1 if binary else 8
        alpha_raw=alpha.convert("1").tobytes() if binary else alpha.tobytes()
        header=struct.pack("<8sIIHBB",b"KFXPX1\0\0",image.width,image.height,len(colours),8,alpha_bits)
        yield header+bytes(v for c in colours for v in c)+indices+alpha_raw
    yield b"KFXRGBA1"+struct.pack("<II",*image.size)+image.tobytes()

def decode_pixel_stream(raw):
    if raw[:8]==b"KFXRGBA1":
        if len(raw)<16:
            raise AssetError("Truncated RGBA stream")
        w,h=struct.unpack_from("<II",raw,8)
        if not 0<w<=1024 or not 0<h<=1024 or len(raw)!=16+w*h*4:
            raise AssetError("Invalid RGBA stream bounds")
        return Image.frombytes("RGBA",(w,h),raw[16:])
    header=struct.Struct("<8sIIHBB")
    if len(raw)<header.size:
        raise AssetError("Truncated indexed stream")
    magic,w,h,count,index_bits,alpha_bits=header.unpack_from(raw)
    if magic!=b"KFXPX1\0\0" or not(0<w<=1024 and 0<h<=1024 and 1<=count<=256) or index_bits!=8 or alpha_bits not in (1,8):
        raise AssetError("Unsupported indexed stream")
    alpha_size=((w+7)//8)*h if alpha_bits==1 else w*h
    if len(raw)!=header.size+count*3+w*h+alpha_size:
        raise AssetError("Indexed stream size differs")
    pos=header.size;palette=raw[pos:pos+count*3];pos+=count*3
    indices=raw[pos:pos+w*h];pos+=w*h
    if max(indices)>=count:
        raise AssetError("Indexed stream palette bounds exceeded")
    image=Image.frombytes("P",(w,h),indices);image.putpalette(palette);image=image.convert("RGBA")
    alpha=Image.frombytes("1" if alpha_bits==1 else "L",(w,h),raw[pos:]).convert("L")
    image.putalpha(alpha)
    return image

def published_png(assets,description,allowlist):
    name=description["file"];relative=safe_relative(name)
    if not name.startswith("assets/creatures/") or not name.endswith(".png") or description!={"file":name,**allowlist.get(name,{})}:
        raise AssetError("Published PNG is not on the repository-relative allowlist")
    path=assets.parents[1]/relative
    no_links(path)
    if path.stat().st_size!=description["bytes"]:
        raise AssetError("Published PNG size differs")
    raw=read_regular(path)
    if digest(raw)!=description["sha256"]:
        raise AssetError("Published PNG hash differs")
    return raw

def reconstruct_member(assets, frame, bank, published_allowlist=None):
    source=frame["source"];kind=source.get("kind")
    if kind=="enhanced":
        if "published" in source:
            raw=published_png(assets,source["published"],published_allowlist or {})
            image=rgba_image(raw)
        elif "pixels" in source:
            image=decode_pixel_stream(load_object(assets,source["pixels"]))
            raw=png_bytes(image,source["encoder"])
        else:
            raw=load_object(assets,source["object"])
            image=rgba_image(raw)
    elif kind=="original":
        if bank is None:
            raise AssetError("Original data is required for fallback frames")
        image=bank.reconstruct(source["recipe"])
        raw=png_bytes(image,source["encoder"])
    elif kind=="transparent":
        size=source["canvas"];colour=source["rgba"]
        if len(size)!=2 or any(type(v) is not int or not 0<v<=1024 for v in size) or len(colour)!=4 or any(type(v) is not int or not 0<=v<=255 for v in colour) or colour[3]!=0:
            raise AssetError("Invalid transparent-frame recipe")
        image=Image.new("RGBA",tuple(size),tuple(colour))
        raw=png_bytes(image,source["encoder"])
    elif kind=="metadata":
        raw=load_object(assets,source["object"])
        image=None
    else:
        raise AssetError("Unknown member source")
    if len(raw)!=frame["bytes"] or digest(raw)!=frame["sha256"]:
        raise AssetError("Member byte mismatch; original PNGs require compatible Pillow/zlib encoder")
    if image is not None and (list(image.size)!=frame["size"] or digest(image.tobytes())!=frame["rgba_sha256"]):
        raise AssetError("PNG pixels/dimensions differ")
    return raw

def rename_new_directory(source,destination):
    if os.name=="nt":
        source.rename(destination)
    elif sys.platform.startswith("linux"):
        libc=ctypes.CDLL(None,use_errno=True);rename=getattr(libc,"renameat2",None)
        if rename is None:
            raise AssetError("Atomic no-replace directory rename unavailable")
        rename.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]
        rename.restype=ctypes.c_int
        if rename(-100,os.fsencode(source),-100,os.fsencode(destination),1):
            code=ctypes.get_errno()
            raise OSError(code,os.strerror(code),str(destination))
    else:
        raise AssetError("Atomic publication supports Windows and Linux")
