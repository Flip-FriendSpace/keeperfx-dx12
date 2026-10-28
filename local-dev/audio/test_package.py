# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the bundled audio without game banks or third-party Python modules."""
import argparse
from collections import Counter
import configparser
import hashlib
import json
from pathlib import Path, PurePosixPath
import wave


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-directory", type=Path, default=repo / "config/mods/dk_modern_audio")
    package = parser.parse_args().package_directory
    manifest = json.loads((package / "local-audio-mod.json").read_text())
    expected = {"speech": 125, "effect": 569, "campaign": 40}
    assert manifest["package"] == "dk_modern_audio"
    assert manifest["status"] == "complete"
    assert manifest["coverage"] == expected
    assert len(manifest["files"]) == sum(expected.values())
    assert Counter(entry["type"] for entry in manifest["files"]) == expected

    paths = set()
    mappings = {"speech": {}, "sounds": {}}
    campaign = set()
    total_bytes = 0
    for entry in manifest["files"]:
        relative = PurePosixPath(entry["file"])
        assert not relative.is_absolute() and ".." not in relative.parts
        assert relative.as_posix() not in paths
        paths.add(relative.as_posix())
        path = package / relative
        data = path.read_bytes()
        assert hashlib.sha256(data).hexdigest() == entry["sha256"], path
        total_bytes += len(data)
        with wave.open(str(path), "rb") as audio:
            assert audio.getcomptype() == "NONE", path
            assert audio.getframerate() == 48000, path
            assert audio.getsampwidth() == 2, path
            assert audio.getnchannels() == entry["channels"], path
            assert audio.getnframes() == entry["frames"], path
        if entry["type"] == "campaign":
            campaign.add(relative.as_posix())
        else:
            section = "sounds" if entry["type"] == "effect" else "speech"
            key = str(entry["id"])
            assert key not in mappings[section]
            mappings[section][key] = "mods/dk_modern_audio/" + relative.as_posix()
    assert paths == {p.relative_to(package).as_posix() for p in package.rglob("*.wav")}
    assert set(mappings["speech"]) == {str(i) for i in range(1, 126)}
    assert campaign == {
        f"campgns/keeporig_eng/{kind}{level:02}.wav"
        for kind in ("good", "bad") for level in range(1, 21)
    }
    config = configparser.ConfigParser()
    config.read(package / "fxdata/sounds.cfg")
    for section, mapping in mappings.items():
        config_section = "speecheng" if section == "speech" else section
        assert dict(config[config_section]) == mapping, config_section
    assert "speech" not in config, "English performances must not override other languages"
    assert (package / "sounds.cfg").read_bytes() == (package / "fxdata/sounds.cfg").read_bytes()
    assert (package / "NOTICE.md").is_file()
    print(f"PASS: {len(paths)} PCM16/48k WAVs, exact coverage/mappings and SHA256 hashes ({total_bytes:,} bytes)")


if __name__ == "__main__":
    main()
