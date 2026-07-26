#!/usr/bin/env python3
"""Behavioral tests for the Luckfox post-image release gate."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "buildroot/board/luckfox-pico/common/post-image.sh"


class PostImageFixtureTests(unittest.TestCase):
    def run_hook(self, binaries: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["sh", str(SCRIPT), str(binaries)],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_renames_required_release_artifacts(self):
        with tempfile.TemporaryDirectory() as tempdir:
            binaries = Path(tempdir)
            (binaries / "rootfs.ubi").write_bytes(b"rootfs")
            (binaries / "uboot-env.bin").write_bytes(b"env")
            (binaries / "temporary.dtb").write_bytes(b"dtb")
            (binaries / "rootfs.ubifs").write_bytes(b"ubifs")

            result = self.run_hook(binaries)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((binaries / "rootfs.img").read_bytes(), b"rootfs")
            self.assertEqual((binaries / "env.img").read_bytes(), b"env")
            self.assertEqual((binaries / "rootfs.ubi").read_bytes(), b"rootfs")
            self.assertEqual((binaries / "uboot-env.bin").read_bytes(), b"env")
            self.assertEqual((binaries / "temporary.dtb").read_bytes(), b"dtb")
            self.assertEqual((binaries / "rootfs.ubifs").read_bytes(), b"ubifs")

            second_result = self.run_hook(binaries)
            self.assertEqual(second_result.returncode, 0, second_result.stderr)
            self.assertEqual((binaries / "rootfs.img").read_bytes(), b"rootfs")
            self.assertEqual((binaries / "env.img").read_bytes(), b"env")

    def test_fails_without_required_release_artifacts(self):
        with tempfile.TemporaryDirectory() as tempdir:
            binaries = Path(tempdir)
            (binaries / "rootfs.ubi").write_bytes(b"rootfs")

            result = self.run_hook(binaries)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("uboot-env.bin", result.stderr)
            self.assertTrue((binaries / "rootfs.ubi").exists())
            self.assertFalse((binaries / "rootfs.img").exists())


if __name__ == "__main__":
    unittest.main()
