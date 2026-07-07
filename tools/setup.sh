#!/usr/bin/env sh
#
# One-time setup for a fresh checkout: fetch git submodules and apply the local
# libssh2 patches. Idempotent — safe to re-run at any time.
#
# Requires git. On Windows, run it from Git Bash.
#
#   ./tools/setup.sh
#
# The patches live in docs/patches/ and are pinned to the submodule SHAs in
# .gitmodules. If a submodule pointer is bumped and a patch stops applying,
# regenerate it:  git -C <submodule> diff > docs/patches/<name>.patch
#
set -eu

root="$(CDPATH= cd "$(dirname "$0")/.." && pwd)"
cd "$root"

echo "==> Fetching submodules (recursive)"
git submodule update --init --recursive

# apply_patch <submodule-dir> <patch-file-name>
apply_patch() {
    sub="$1"
    name="$2"
    patch="$root/docs/patches/$name"

    if git -C "$sub" apply -p1 --reverse --check "$patch" >/dev/null 2>&1; then
        echo "    [skip] $name (already applied in $sub)"
    elif git -C "$sub" apply -p1 --check "$patch" >/dev/null 2>&1; then
        git -C "$sub" apply -p1 "$patch"
        echo "    [ok]   $name -> $sub"
    else
        echo "    [FAIL] $name does not apply cleanly in $sub" >&2
        echo "           The submodule SHA may have moved; regenerate the patch." >&2
        exit 1
    fi
}

echo "==> Applying libssh2 patches"
apply_patch components/libssh2_esp          libssh2_esp-sim-build.patch
apply_patch components/libssh2_esp/libssh2  libssh2-ram-diet.patch

echo "==> Done. Build with 'idf.py build' (device) or 'cmake --preset sim-windows' (sim)."
