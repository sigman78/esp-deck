#!/usr/bin/env sh
#
# One-time setup for a fresh checkout: fetch git submodules.
#
# libssh2 is NOT a submodule — it is cloned and patched automatically by CMake
# at configure time (see components/libssh2_esp/). The only submodule left is
# esp_littlefs, so this is just a convenience wrapper around:
#
#   git submodule update --init --recursive
#
# Requires git. On Windows, run it from Git Bash. Safe to re-run.
#
set -eu

root="$(CDPATH= cd "$(dirname "$0")/.." && pwd)"
cd "$root"

echo "==> Fetching submodules (esp_littlefs)"
git submodule update --init --recursive

echo "==> Done. Build with 'idf.py build' (device) or 'cmake --preset sim-windows' (sim)."
echo "    CMake fetches and patches libssh2 on the first configure."
