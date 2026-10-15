# SPDX-License-Identifier: GPL-2.0-or-later
"""Build enhanced creature mods using enhanced assets and player-owned fallbacks.

Requires Pillow. Never changes game settings or enables a mod. Installation is
transactional: a complete, checked sibling directory is renamed into place.
"""
from __future__ import annotations
import argparse
import ctypes
import sys
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tempfile
import zipfile
from PIL import Image

IDENTITY = "original-heroes-and-imp-4x-v1"
KIND = "keeperfx-enhanced-sprites-install"
RECEIPT = "install-receipt.json"
HEADER = struct.Struct("<I8Bhh")


class InstallError(ValueError):
    """Input or installed output failed validation."""


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_relative(value: str) -> Path:
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise InstallError(f"Unsafe relative path: {value!r}")
    parts = value.split("/")
    if any(p in ("", ".", "..") or p.endswith((".", " ")) or
           any(ord(c) < 32 for c in p) or
           re.match(r"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", p, re.I)
           for p in parts) or PurePosixPath(value).is_absolute():
        raise InstallError(f"Unsafe relative path: {value!r}")
    return Path(*parts)


def no_links(path: Path) -> None:
    # Inspect every existing component, including junctions on Windows.
    for item in (path, *path.parents):
        if item.is_symlink() or (hasattr(item, "is_junction") and item.is_junction()):
            raise InstallError(f"Links/junctions are not permitted: {item}")


def read_regular(path: Path) -> bytes:
    no_links(path)
    if not path.is_file():
        raise InstallError(f"Required regular file missing: {path}")
    return path.read_bytes()


def parse_json(data: bytes, label: str):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise InstallError(f"Duplicate JSON key in {label}: {key}")
            result[key] = value
        return result
    try:
        return json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise InstallError(f"Invalid JSON: {label}") from error


def load_manifest(assets: Path):
    raw = read_regular(assets / "manifest.json")
    manifest = parse_json(raw, "manifest.json")
    if manifest.get("schema") != 1 or manifest.get("id") != IDENTITY:
        raise InstallError("Unsupported asset manifest identity/schema")
    creatures = manifest.get("creatures")
    if not isinstance(creatures, list) or not creatures:
        raise InstallError("Manifest needs a nonempty creature list")
    slugs = set()
    for item in creatures:
        slug = item.get("slug", "")
        if not isinstance(slug, str) or not re.fullmatch(r"[a-z][a-z_]*", slug) or slug in slugs:
            raise InstallError("Invalid or duplicate creature slug")
        slugs.add(slug)
        safe_relative(item["recipe"])
    return manifest, digest(raw)


class OriginalFrames:
    def __init__(self, data: Path):
        # The explicitly selected read-only data root may be the runtime junction.
        # Resolve that root once; files and descendants still reject links.
        data = data.resolve(strict=True)
        self.tab = read_regular(data / "creature.tab")
        self.blob = read_regular(data / "creature.jty")
        self.palette = read_regular(data / "palette.dat")
        if not self.tab or len(self.tab) % HEADER.size:
            raise InstallError("Creature table must contain complete 16-byte records")
        if len(self.palette) != 768 or max(self.palette) > 63:
            raise InstallError("Original palette must contain 768 six-bit RGB values")
        self.source_hashes = {"creature.tab": digest(self.tab), "creature.jty": digest(self.blob),
                              "palette.dat": digest(self.palette)}
        self.cache = {}

    def decode(self, index: int):
        if type(index) is not int or index < 0 or index >= len(self.tab) // HEADER.size:
            raise InstallError("Original table index out of bounds")
        if index in self.cache:
            return self.cache[index]
        header = HEADER.unpack_from(self.tab, index * HEADER.size)
        start, width, height, frame_w, frame_h, _, _, offset_x, offset_y, _, _ = header
        end = (HEADER.unpack_from(self.tab, (index + 1) * HEADER.size)[0]
               if (index + 1) * HEADER.size < len(self.tab) else len(self.blob))
        if not (0 <= start < end <= len(self.blob)):
            raise InstallError("Original sprite byte bounds are invalid")
        if not (width and height and frame_w and frame_h):
            raise InstallError("Original sprite has empty dimensions")
        if offset_x + width > frame_w or offset_y + height > frame_h:
            raise InstallError("Original crop exceeds its frame")
        encoded = memoryview(self.blob)[start:end]
        pixels = bytearray(width * height * 4)
        pos = 0
        for y in range(height):
            x = 0
            while True:
                if pos >= len(encoded):
                    raise InstallError("Truncated original sprite row")
                command = encoded[pos]
                pos += 1
                if command == 0:
                    break
                run = command if command < 128 else 256 - command
                visible = min(run, width - x)
                if command < 128:
                    if pos + run > len(encoded):
                        raise InstallError("Truncated original literal run")
                    for n in range(visible):
                        colour = encoded[pos + n] * 3
                        at = (y * width + x + n) * 4
                        pixels[at:at + 4] = bytes((self.palette[colour] * 4,
                            self.palette[colour + 1] * 4, self.palette[colour + 2] * 4, 255))
                    pos += run
                x += visible
        cropped = Image.frombytes("RGBA", (width, height), bytes(pixels))
        image = Image.new("RGBA", (frame_w, frame_h))
        image.alpha_composite(cropped, (offset_x, offset_y))
        rgba_hash = digest(image.tobytes())
        buffer = io.BytesIO()
        image.save(buffer, format="PNG")
        self.cache[index] = (list(header), rgba_hash, buffer.getvalue())
        return self.cache[index]

    def png(self, specification: dict) -> bytes:
        header, rgba_hash, png = self.decode(specification["table_index"])
        if header != specification["header"] or rgba_hash != specification["rgba_sha256"]:
            raise InstallError("Local original sprite does not match the supported source")
        return png


def frame_order(sprites):
    seen = set()
    order = []
    if not isinstance(sprites, list) or not sprites:
        raise InstallError("Recipe needs sprite entries")
    for sprite in sprites:
        for view in ("td", "fp"):
            for direction in sprite[view]:
                for frame in direction:
                    name = frame["file"]
                    safe_relative(name)
                    if name == "sprites.json" or name.casefold() == RECEIPT.casefold():
                        raise InstallError("Reserved frame archive path")
                    if name.casefold() in seen:
                        # Aliased frames may repeat exact archive paths.
                        if name not in order:
                            raise InstallError("Case-colliding frame archive path")
                        continue
                    seen.add(name.casefold())
                    order.append(name)
    return order


def png_checked(raw: bytes) -> bytes:
    try:
        with Image.open(io.BytesIO(raw)) as image:
            if image.format != "PNG" or not (0 < image.width <= 1024 and 0 < image.height <= 1024):
                raise InstallError("Enhanced frame must be PNG with axes in 1..1024")
            image.load()
    except (OSError, Image.DecompressionBombError) as error:
        raise InstallError("Invalid enhanced PNG") from error
    return raw


def write_zip(path: Path, frames, sprites):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, raw in [*frames, ("sprites.json", json.dumps(sprites, separators=(",", ":")).encode())]:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, raw, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def verify_output(output: Path, manifest, manifest_hash: str):
    receipt = parse_json(read_regular(output / RECEIPT), RECEIPT)
    if (receipt.get("schema"), receipt.get("id"), receipt.get("kind"), receipt.get("manifest_sha256")) != (
            1, IDENTITY, KIND, manifest_hash):
        raise InstallError("Output receipt identity does not match these assets")
    expected = {f"{folder}/{item['slug']}{suffix}" for item in manifest["creatures"]
                for folder, suffix in (("fxdata", "-sprites.zip"), ("creatrs", ".cfg"))}
    files = receipt.get("files")
    if not isinstance(files, dict) or set(files) != expected:
        raise InstallError("Output receipt has an unexpected file list")
    actual = set()
    actual_dirs = set()
    for directory, dirs, names in os.walk(output, followlinks=False):
        for name in [*dirs, *names]:
            no_links(Path(directory) / name)
        for name in dirs:
            actual_dirs.add((Path(directory) / name).relative_to(output).as_posix())
        for name in names:
            actual.add((Path(directory) / name).relative_to(output).as_posix())
    if actual != expected | {RECEIPT} or actual_dirs != {"fxdata", "creatrs"}:
        raise InstallError("Output contains unexpected or missing files")
    for name, sha in files.items():
        if digest(read_regular(output / safe_relative(name))) != sha:
            raise InstallError(f"Installed file hash mismatch: {name}")
    return receipt


def rename_new_directory(source: Path, destination: Path) -> None:
    """Atomically publish without replacing a concurrently created directory."""
    if os.name == "nt":
        source.rename(destination)  # Windows rename fails if destination exists.
    elif sys.platform.startswith("linux"):
        libc = ctypes.CDLL(None, use_errno=True)
        rename = getattr(libc, "renameat2", None)
        if rename is None:
            raise InstallError("Atomic no-replace directory rename is unavailable")
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        rename.restype = ctypes.c_int
        if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1):
            code = ctypes.get_errno()
            raise OSError(code, os.strerror(code), str(destination))
    else:
        raise InstallError("Atomic no-replace installation supports Windows and Linux")


def build(assets: Path, data: Path | None, output: Path, verify_only=False):
    assets, output = assets.absolute(), output.absolute()
    no_links(output)
    manifest, manifest_hash = load_manifest(assets)
    if output.exists() and not output.is_dir():
        raise InstallError("Output must be a directory")
    if verify_only:
        return verify_output(output, manifest, manifest_hash)
    if output.exists() and any(output.iterdir()):
        return verify_output(output, manifest, manifest_hash)
    if data is None:
        raise InstallError("--data is required when building")
    originals = OriginalFrames(data.absolute())
    # Validate parent ownership/path before creating a sibling staging directory.
    no_links(output.parent)
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="." + output.name + "-", dir=output.parent))
    try:
        (stage / "fxdata").mkdir()
        (stage / "creatrs").mkdir()
        files = {}
        for item in manifest["creatures"]:
            raw = read_regular(assets / safe_relative(item["recipe"]))
            if digest(raw) != item["sha256"]:
                raise InstallError(f"Recipe hash mismatch: {item['slug']}")
            recipe = parse_json(raw, item["recipe"])
            if recipe.get("schema") != 1 or recipe.get("slug") != item["slug"]:
                raise InstallError("Recipe identity does not match manifest")
            order = frame_order(recipe["sprites"])
            if set(order) != set(recipe["frames"]):
                raise InstallError("Recipe frame map differs from sprite references")
            frames = []
            for name in order:
                description = recipe["frames"][name]
                if set(description) == {"enhanced", "sha256"}:
                    image = read_regular(assets / safe_relative(description["enhanced"]))
                    if digest(image) != description["sha256"]:
                        raise InstallError(f"Enhanced PNG hash mismatch: {name}")
                    frames.append((name, png_checked(image)))
                elif set(description) == {"original"}:
                    frames.append((name, originals.png(description["original"])))
                else:
                    raise InstallError("Unknown frame source specification")
            slug = item["slug"]
            zip_name, cfg_name = f"fxdata/{slug}-sprites.zip", f"creatrs/{slug}.cfg"
            write_zip(stage / zip_name, frames, recipe["sprites"])
            config = recipe["config"]
            if not isinstance(config, str) or not re.match(r"\A\[sprites\]\r?\n", config) or "\x00" in config:
                raise InstallError("Recipe config must be a sprite override")
            for line in config.splitlines()[1:]:
                if line.strip() and not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*\s*=\s*[A-Za-z_][A-Za-z0-9_]*", line.strip()):
                    raise InstallError("Recipe config must contain only sprite assignments")
            (stage / cfg_name).write_bytes(config.encode("utf-8"))
            for name in (zip_name, cfg_name):
                files[name] = digest((stage / name).read_bytes())
        receipt = {"schema": 1, "id": IDENTITY, "kind": KIND, "manifest_sha256": manifest_hash,
                   "files": files, "fallback_source_sha256": originals.source_hashes}
        (stage / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        verify_output(stage, manifest, manifest_hash)
        if output.exists():
            no_links(output)
            output.rmdir()  # Only an empty directory can be removed.
        # rename refuses an existing directory on Windows; recheck for all hosts.
        if output.exists():
            raise InstallError("Output appeared while building")
        rename_new_directory(stage, output)
        stage = None
        return receipt
    finally:
        if stage is not None:
            no_links(stage)
            if stage.resolve().parent != output.parent.resolve() or not stage.name.startswith('.' + output.name + '-'):
                raise InstallError("Refusing cleanup outside the owned staging directory")
            shutil.rmtree(stage)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=Path(__file__).resolve().parents[2] / "assets/creatures")
    parser.add_argument("--data", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        receipt = build(args.assets, args.data, args.output, args.verify_only)
    except (InstallError, OSError, KeyError, TypeError, struct.error, zipfile.BadZipFile) as error:
        parser.exit(1, f"Enhanced sprites: {error}\n")
    print(json.dumps({"verified": True, "output": str(args.output), "manifest_sha256": receipt["manifest_sha256"]}))


if __name__ == "__main__":
    main()
