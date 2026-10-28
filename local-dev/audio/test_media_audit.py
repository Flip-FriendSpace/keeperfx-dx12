# SPDX-License-Identifier: GPL-2.0-or-later
"""An interrupted traversal must never leave a passing media-audit receipt."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from audit_original_media import audit


class MediaAuditReceipts(unittest.TestCase):
    def fixture(self, root):
        original, runtime = root / "original", root / "runtime"
        names = [(f"keeper{i:02}.ogg", f"music/keeper{i:02}.ogg") for i in range(2, 8)]
        names += [(f"ldata/{name}.smk", f"ldata/{name}.smk")
                  for name in ("intromix", "outromix", "drag")]
        for source, target in names:
            for file in (original / source, runtime / target):
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(source.encode())
        return original, runtime

    def test_complete_integrity_is_not_decoder_qualification(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            report = audit(*self.fixture(root), root / "report")
            self.assertTrue(report["passed"])
            self.assertTrue(report["completed"])
            self.assertEqual(len(report["files"]), 9)
            self.assertFalse(report["decoder_completed"])
            self.assertFalse(report["live_playback_qualified"])

    def test_changed_source_bytes_fail(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            original, runtime = self.fixture(root)
            (runtime / "music/keeper02.ogg").write_bytes(b"different")
            report = audit(original, runtime, root / "report")
            self.assertTrue(report["completed"])
            self.assertFalse(report["passed"])

    def test_interrupted_decoder_leaves_incomplete_failure(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            original, runtime = self.fixture(root)
            good = subprocess.CompletedProcess([], 0, "", "")
            with patch("audit_original_media.subprocess.run",
                       side_effect=[good, subprocess.TimeoutExpired("decoder", 180)]):
                with self.assertRaises(subprocess.TimeoutExpired):
                    audit(original, runtime, root / "report", ffmpeg=Path("decoder"))
            report = json.loads((root / "report/report.json").read_text())
            self.assertEqual(len(report["files"]), 1)
            self.assertFalse(report["completed"])
            self.assertFalse(report["passed"])
            self.assertFalse(report["decoder_completed"])


if __name__ == "__main__":
    unittest.main()
