#!/usr/bin/env bash
set -euo pipefail

if command -v apt-get >/dev/null 2>&1; then
    apt-get update
    apt-get install -y --no-install-recommends \
        build-essential \
        pkg-config \
        libgit2-dev \
        libfuse3-dev
else
    echo "setup.sh: unsupported distro (no apt-get)." >&2
    echo "Install: a C11 compiler (cc/gcc), GNU make, pkg-config, libgit2-dev (>=1.7), libfuse3-dev." >&2
    exit 1
fi