# SPDX-License-Identifier: GPL-2.0-only
"""Pin the generated V67 payload against the committed manifest.

This is the part of the former ``test_packaging.py`` that does not depend on
the Linux packaging tree. The install-layout and native-package checks were
dropped together with ``debian/``, ``packaging/`` and the kbuild files; the
original file is archived in ``docs/linux-legacy/test_packaging.py``.
"""
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "firmware"))
import generate_v67  # noqa: E402


class FirmwareManifestTests(unittest.TestCase):
    @staticmethod
    def manifest():
        return json.loads((ROOT / "firmware/artifacts-610.43.03.json").read_text())

    def test_v67_payload_matches_pin(self):
        digest = hashlib.sha256(generate_v67.build_payload()).hexdigest()
        self.assertEqual(digest, generate_v67.PAYLOAD_SHA256)
        self.assertEqual(digest, self.manifest()["generated_payload"]["sha256"])
