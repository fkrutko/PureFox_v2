#!/usr/bin/env python3
"""Behavioral regression tests for patch-webui.sh."""

from pathlib import Path
import os
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "ext_tree/board/luckfox/scripts/patch-webui.sh"


COMMON_HTML = """<html>
<head><title>{title} 1.0</title></head>
<body>
<input id="radio_fm" type="radio" name="pm">
<input id="check_preload" type="checkbox"> Full preloading
<input id="mmap" name="mmap" type="radio" checked>
<input id="rw" name="mmap" type="radio">
<input id="check_memory" type="checkbox" checked>
<input id="cores0" type="radio" name="cores" checked>
<input id="cores2" type="radio" name="cores">
<input id="CardNum" style="width:24px">
{id_extra}
</body>
</html>
"""

DIMAS_HTML = """<html>
<head><title>Album Player 1.0</title></head>
<body>
<input id="radio_fm" type="radio" name="pm">
<input id="check_preload" type="checkbox">
<input id="mmap" name="mmap" type="radio" checked>
<input id="rw" name="mmap" type="radio">
<input id="check_memory" type="checkbox" checked>
<input id="cores0" type="radio" name="cores" checked>
<input id="cores2" type="radio" name="cores">
<input id="CardNum" style="width:24px">
</body>
</html>
"""


class PatchWebUiFixtureTests(unittest.TestCase):
    def make_archive(self, root: Path, name: str, files: dict[str, str]) -> Path:
        source = root / name
        bundle = source / "bundle"
        for relative_path, content in files.items():
            path = bundle / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

        archive = root / f"{name}.tar.gz"
        with tarfile.open(archive, "w:gz") as handle:
            handle.add(bundle, arcname="bundle")
        return archive

    def make_overlay(self, root: Path) -> Path:
        overlay = root / "overlay"
        targets = {
            "usr/aprenderer/renderer.html": "renderer-before",
            "usr/aplayer/aplayer.html": "aplayer-before",
            "usr/aplayer/dimas/aplayer.html": "dimas-before",
        }
        for relative_path, content in targets.items():
            path = overlay / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        return overlay

    def run_patch(self, overlay: Path, aplayer: Path, renderer: Path) -> subprocess.CompletedProcess[str]:
        environment = os.environ | {"OVERLAY": str(overlay)}
        return subprocess.run(
            ["bash", str(SCRIPT), str(aplayer), str(renderer), "2.18"],
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )

    def test_publishes_all_validated_pages(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            overlay = self.make_overlay(root)
            aplayer = self.make_archive(
                root,
                "aplayer",
                {
                    "aplayer.html": COMMON_HTML.format(
                        title="Album Player",
                        id_extra='<tr><td id="radio_pict">(1-10)</td></tr>',
                    ),
                    "dimas/aplayer.html": DIMAS_HTML,
                },
            )
            renderer = self.make_archive(
                root,
                "renderer",
                {
                    "renderer.html": COMMON_HTML.format(
                        title="APlayer Media Renderer",
                        id_extra="",
                    ),
                },
            )

            result = self.run_patch(overlay, aplayer, renderer)
            self.assertEqual(result.returncode, 0, result.stderr)

            self.assertIn("<title>Album Player 2.18</title>", (overlay / "usr/aplayer/aplayer.html").read_text())
            self.assertIn('id="api_cx"', (overlay / "usr/aplayer/aplayer.html").read_text())
            self.assertIn('id="api_key"', (overlay / "usr/aplayer/aplayer.html").read_text())
            self.assertIn("<title>Album Player 2.18</title>", (overlay / "usr/aplayer/dimas/aplayer.html").read_text())
            self.assertIn("<title>APlayer Media Renderer 2.18</title>", (overlay / "usr/aprenderer/renderer.html").read_text())

    def test_template_drift_leaves_overlay_unchanged(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            overlay = self.make_overlay(root)
            aplayer = self.make_archive(
                root,
                "aplayer",
                {
                    "aplayer.html": COMMON_HTML.format(title="Album Player", id_extra=""),
                    "dimas/aplayer.html": DIMAS_HTML,
                },
            )
            renderer = self.make_archive(
                root,
                "renderer",
                {
                    "renderer.html": COMMON_HTML.format(
                        title="APlayer Media Renderer",
                        id_extra="",
                    ),
                },
            )

            before = {
                path: (overlay / path).read_text()
                for path in (
                    "usr/aprenderer/renderer.html",
                    "usr/aplayer/aplayer.html",
                    "usr/aplayer/dimas/aplayer.html",
                )
            }
            result = self.run_patch(overlay, aplayer, renderer)
            self.assertNotEqual(result.returncode, 0)
            for path, content in before.items():
                self.assertEqual((overlay / path).read_text(), content)


if __name__ == "__main__":
    unittest.main()
