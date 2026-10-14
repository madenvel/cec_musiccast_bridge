#!/bin/sh
# Build on Debian/Raspberry Pi OS; dpkg-buildpackage checks build dependencies.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
exec dpkg-buildpackage -us -uc -b "$@"
