#!/bin/sh
# apt resolves dependencies; package maintainer scripts manage the service.
set -eu
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /path/to/cec-musiccast-bridge_VERSION_ARCH.deb" >&2
    exit 2
fi
package=$(realpath -- "$1")
if [ "$(id -u)" -eq 0 ]; then
    exec apt-get install "$package"
fi
exec sudo apt-get install "$package"
