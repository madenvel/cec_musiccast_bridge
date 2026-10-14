#!/usr/bin/env python3
"""Stamp a Debian changelog with the checked-out release tag's version."""
import argparse
from datetime import datetime, timezone
from email.utils import format_datetime
from pathlib import Path
import re
import subprocess


def debian_version(tag):
    number = r"(?:0|[1-9][0-9]*)"
    match = re.fullmatch(
        rf"v?({number}\.{number}\.{number})(?:-([0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*))?",
        tag,
    )
    if not match:
        raise ValueError("Expected a tag such as v1.2.3, 1.2.3 or v1.2.3-rc.1")
    version, prerelease = match.groups()
    # Debian's tilde sorts prereleases before the corresponding final release.
    return version + ("~" + prerelease if prerelease else "") + "-1"


def stamp_changelog(path, tag, timestamp):
    version = debian_version(tag)
    original = path.read_text()
    header = re.match(r"cec-musiccast-bridge \(([^)]+)\) ", original)
    maintainer = re.search(r"^ -- (.+?)  .+$", original, re.MULTILINE)
    if not header or not maintainer:
        raise ValueError("Unexpected debian/changelog format")
    if header.group(1) != version:
        date = format_datetime(datetime.fromtimestamp(timestamp, timezone.utc))
        entry = (
            f"cec-musiccast-bridge ({version}) unstable; urgency=medium\n\n"
            f"  * Build release {tag}.\n\n"
            f" -- {maintainer.group(1)}  {date}\n\n"
        )
        path.write_text(entry + original)
    return version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="Version tag of the checked-out source")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        # Use commit time so rerunning a tag does not change package timestamps.
        timestamp = int(subprocess.check_output(
            ["git", "show", "-s", "--format=%ct", "HEAD"], cwd=root, text=True
        ).strip())
        print(stamp_changelog(root / "debian/changelog", args.tag, timestamp))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"Error: {error}\n")


if __name__ == "__main__":
    main()
