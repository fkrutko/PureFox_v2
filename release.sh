#!/bin/sh
#
# Build PureFox on this machine and publish it as the "latest" GitHub release,
# the same assets the CI workflow produces:
#   PureFox-<build>.zip   images for RV1106_Toolkit (env, idblock, uboot, boot, rootfs)
#   rootfs.squashfs       root filesystem for the online update (/opt/update.sh)
#   boot.img              kernel + DTB for the online update
#   version.txt, SHA256SUMS
#
#   ./release.sh            build HEAD and publish v<base>.<commit count>
#   ./release.sh --force    replace an existing release with the same build id
#   ./release.sh --dirty    allow uncommitted changes (build id gets -dirty)
#
# Needs: gh (GitHub CLI) logged in with write access to the repo (gh auth login).

set -eu

REPO="fkrutko/PureFox_v2"
FORCE=0
DIRTY=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --force) FORCE=1 ;;
        --dirty) DIRTY=1 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "Usage: $0 [--force] [--dirty]" >&2; exit 1 ;;
    esac
    shift
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

command -v gh >/dev/null 2>&1 || { echo "gh not found (dnf install gh / apt install gh)" >&2; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "gh is not logged in: run gh auth login" >&2; exit 1; }

# The release points at a commit, so it must already be on GitHub.
SHA=$(git rev-parse HEAD)
git fetch -q origin
if [ -z "$(git branch -r --contains "$SHA" | grep '^ *origin/')" ]; then
    echo "Commit ${SHA%"${SHA#????????}"} is not on origin yet: git push first" >&2
    exit 1
fi

SUFFIX=
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    [ "$DIRTY" -eq 1 ] || { echo "Uncommitted changes: commit them or use --dirty" >&2; exit 1; }
    SUFFIX=-dirty
fi

BASE=$(sed -n "s/.*PUREFOX_BASE_VERSION', '\([^']*\)'.*/\1/p" \
       ext_tree/board/luckfox/rootfs_overlay/var/www/config.php)
BUILD="${BASE:-2.8}.$(git rev-list --count HEAD)$SUFFIX"
TAG="v$BUILD"

if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
    [ "$FORCE" -eq 1 ] || { echo "Release $TAG already exists (use --force to replace it)" >&2; exit 1; }
fi

# build.sh defaults to Debian's llvm-14 path; find libclang elsewhere (Fedora: /usr/lib64)
if [ -z "${LIBCLANG_PATH:-}" ] && [ ! -d /usr/lib/llvm-14/lib ]; then
    lib=$(ls /usr/lib64/libclang.so* /usr/lib/llvm-*/lib/libclang.so* /usr/lib/x86_64-linux-gnu/libclang*.so* 2>/dev/null | head -n 1)
    [ -n "$lib" ] && LIBCLANG_PATH=$(dirname "$lib") && export LIBCLANG_PATH
fi

echo "=== Building PureFox $BUILD (commit $SHA) ==="
PUREFOX_BUILD="$BUILD" ./build.sh

I=buildroot/output/images
OUT=buildroot/output/release
rm -rf "$OUT"
mkdir -p "$OUT/flash"
for f in env.img idblock.img uboot.img boot.img rootfs.img rootfs.squashfs; do
    [ -f "$I/$f" ] || { echo "Missing $I/$f" >&2; exit 1; }
done
grep -qx "$BUILD" buildroot/output/target/etc/purefox-release \
    || { echo "Build id in the rootfs is not $BUILD" >&2; exit 1; }

cp "$I/env.img" "$I/idblock.img" "$I/uboot.img" "$I/boot.img" "$I/rootfs.img" "$OUT/flash/"
(cd "$OUT/flash" && zip -q -9 "../PureFox-$BUILD.zip" ./*)
rm -rf "$OUT/flash"
cp "$I/rootfs.squashfs" "$I/boot.img" "$OUT/"
echo "$BUILD" > "$OUT/version.txt"
(cd "$OUT" && sha256sum "PureFox-$BUILD.zip" rootfs.squashfs boot.img > SHA256SUMS)
ls -la "$OUT"

[ "$FORCE" -eq 1 ] && gh release delete "$TAG" -R "$REPO" --yes --cleanup-tag >/dev/null 2>&1 || true

echo "=== Publishing $TAG ==="
gh release create "$TAG" -R "$REPO" \
    --target "$SHA" \
    --title "PureFox $BUILD" \
    --latest \
    --notes "PureFox $BUILD (commit $SHA)

Full flash with RV1106_Toolkit: unzip \`PureFox-$BUILD.zip\`,
Download → USB → firmware folder → check all → Download.

Online update: the version button in the web interface (uses rootfs.squashfs and boot.img from here)." \
    "$OUT/PureFox-$BUILD.zip" \
    "$OUT/rootfs.squashfs" \
    "$OUT/boot.img" \
    "$OUT/version.txt" \
    "$OUT/SHA256SUMS"

echo "Published: https://github.com/$REPO/releases/tag/$TAG"
