# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify installed original films/music and optionally decode every audio/video frame.

This records asset integrity and decoder coverage, not live timing or audible playback.
It reads media in place and writes only the requested report directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def audit(original, runtime, output, ffmpeg=None, ffprobe=None):
    original, runtime = original.resolve(), runtime.resolve()
    output.mkdir(parents=True, exist_ok=False)
    pairs = [(f"keeper{index:02}.ogg", f"music/keeper{index:02}.ogg") for index in range(2, 8)]
    pairs += [(f"ldata/{name}.smk", f"ldata/{name}.smk") for name in ("intromix", "outromix", "drag")]
    report = {"schema": 1,
              "scope": "original-byte integrity" + (" and complete decoder traversal" if ffmpeg else ""),
              "decoder_requested": bool(ffmpeg), "decoder_completed": False,
              "live_playback_qualified": False, "expected_files": len(pairs),
              "completed": False, "files": [], "passed": False}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for source_name, runtime_name in pairs:
        source, installed = original / source_name, runtime / runtime_name
        entry = {"source": str(source), "installed": str(installed), "present": source.is_file() and installed.is_file()}
        if entry["present"]:
            entry.update(source_sha256=digest(source), installed_sha256=digest(installed), bytes=installed.stat().st_size)
            entry["original_bytes_match"] = entry["source_sha256"] == entry["installed_sha256"]
            if ffprobe:
                process = subprocess.run([str(ffprobe), "-v", "error", "-count_frames", "-show_streams",
                                          "-show_format", "-of", "json", str(installed)],
                                         capture_output=True, text=True, timeout=180)
                entry["probe_exit"] = process.returncode
                entry["probe_errors"] = process.stderr
                if process.returncode == 0:
                    entry["media"] = json.loads(process.stdout)
            if ffmpeg:
                process = subprocess.run([str(ffmpeg), "-nostdin", "-v", "error", "-xerror",
                    "-err_detect", "explode", "-i", str(installed), "-map", "0:v?", "-map", "0:a?",
                    "-f", "null", "-"], capture_output=True, text=True, timeout=180)
                entry["full_decode_exit"] = process.returncode
                entry["decode_errors"] = process.stderr
        entry["passed"] = bool(entry["present"] and entry.get("original_bytes_match") and
                               entry.get("probe_exit", 0) == 0 and entry.get("full_decode_exit", 0) == 0)
        report["files"].append(entry)
        report["completed"] = len(report["files"]) == len(pairs)
        report["passed"] = report["completed"] and all(item["passed"] for item in report["files"])
        report["decoder_completed"] = bool(ffmpeg and len(report["files"]) == len(pairs) and
            all(item.get("full_decode_exit") == 0 for item in report["files"]))
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"{'PASS' if entry['passed'] else 'FAIL'} {runtime_name}", flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--ffprobe", type=Path)
    args = parser.parse_args()
    report = audit(args.original, args.runtime, args.output, args.ffmpeg, args.ffprobe)
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
