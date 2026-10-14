"""Check release tag conversion and the changelog used by dpkg-buildpackage."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "package_version", ROOT / "scripts/prepare-debian-version.py"
)
VERSION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERSION)


class PackageVersionTests(unittest.TestCase):
    def test_release_tags(self):
        for tag, expected in (("v0.3.0", "0.3.0-1"), ("12.34.56", "12.34.56-1"),
                              ("v1.0.0-rc.1", "1.0.0~rc.1-1")):
            with self.subTest(tag=tag):
                self.assertEqual(VERSION.debian_version(tag), expected)

    def test_invalid_tags(self):
        for tag in ("main", "v1.2", "v01.2.3", "v1.2.3\n", "v1.2.3/other",
                    "v1.2.3-", "v1.2.3$(echo bad)", "--help"):
            with self.subTest(tag=tag):
                with self.assertRaises(ValueError):
                    VERSION.debian_version(tag)

    def test_prerelease_sorts_before_release(self):
        subprocess.run(["dpkg", "--compare-versions",
                        VERSION.debian_version("v1.0.0-rc.1"), "lt",
                        VERSION.debian_version("v1.0.0")], check=True)

    def test_changelog_stamping_is_valid_and_idempotent(self):
        original = (ROOT / "debian/changelog").read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "changelog"
            path.write_text(original)
            expected = VERSION.stamp_changelog(path, "v9.8.7-rc.1", 1780000000)
            stamped = path.read_text()
            self.assertTrue(stamped.endswith(original))
            self.assertIn("Build release v9.8.7-rc.1", stamped)
            parsed = subprocess.check_output(
                ["dpkg-parsechangelog", "--file", str(path), "--show-field", "Version"],
                text=True,
            ).strip()
            self.assertEqual(parsed, expected)
            VERSION.stamp_changelog(path, "v9.8.7-rc.1", 1780000001)
            self.assertEqual(path.read_text(), stamped)

    def test_bad_tag_leaves_changelog_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "changelog"
            path.write_text("original content")
            with self.assertRaises(ValueError):
                VERSION.stamp_changelog(path, "main", 1780000000)
            self.assertEqual(path.read_text(), "original content")
