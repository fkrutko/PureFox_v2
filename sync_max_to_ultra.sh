#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SRC_BRANCH="MAX_6.X"
DST_BRANCH="ULTRA_6.X"
SYNC_SUCCESS=0
ON_DESTINATION_BRANCH=0
DST_HEAD=""

cleanup() {
    status=$?
    trap - EXIT

    if [ "$status" -ne 0 ] && [ "$SYNC_SUCCESS" -eq 0 ] && [ "$ON_DESTINATION_BRANCH" -eq 1 ]; then
        echo "ERROR: Sync failed; restoring $DST_BRANCH and returning to $SRC_BRANCH..." >&2
        if [ -n "$DST_HEAD" ]; then
            git reset --hard "$DST_HEAD" >/dev/null 2>&1 || \
                echo "WARNING: Could not restore $DST_BRANCH to $DST_HEAD." >&2
            git clean -fd >/dev/null 2>&1 || \
                echo "WARNING: Could not remove untracked files created during sync." >&2
        fi
        git checkout "$SRC_BRANCH" >/dev/null 2>&1 || \
            echo "WARNING: Could not return to $SRC_BRANCH automatically; inspect git status." >&2
    fi

    exit "$status"
}
trap cleanup EXIT

echo "=== Syncing $SRC_BRANCH to $DST_BRANCH (preserving Ultra-specific files) ==="

# Check we're on source branch
CURRENT_BRANCH=$(git branch --show-current)
if [ "$CURRENT_BRANCH" != "$SRC_BRANCH" ]; then
    echo "ERROR: Must be on $SRC_BRANCH branch. Currently on: $CURRENT_BRANCH"
    exit 1
fi

# Check working tree is clean, including untracked files that checkout could overwrite.
if [ -n "$(git status --porcelain=v1 --untracked-files=all)" ]; then
    echo "ERROR: Working tree has uncommitted changes. Commit or stash them first."
    exit 1
fi

git rev-parse --verify --quiet "$SRC_BRANCH^{commit}" >/dev/null || {
    echo "ERROR: Source branch not found: $SRC_BRANCH"
    exit 1
}
git rev-parse --verify --quiet "$DST_BRANCH^{commit}" >/dev/null || {
    echo "ERROR: Destination branch not found: $DST_BRANCH"
    exit 1
}

# Ultra-specific files: storage type (eMMC vs MTD), DTS, boot, platform scripts
EXCLUDE_PATTERNS=(
    # Sync script itself (MAX only)
    "^sync_max_to_ultra.sh$"

    # Build scripts (different storage layout)
    "^build.sh$"
    "^buildroot/board/luckfox-pico/"
    "^ext_tree/configs/"
    "^ext_tree/external.mk$"

    # Platform-specific DTS (MAX vs Ultra)
    "^ext_tree/board/luckfox/dts_max/"

    # Build hooks (different post-build for MAX vs Ultra)
    "^ext_tree/board/luckfox/scripts/post-build.sh$"
    "^ext_tree/board/luckfox/scripts/post-image"
    "^ext_tree/board/luckfox/scripts/linux-post-build.sh$"

    # U-boot binaries (platform-specific)
    "^ext_tree/board/luckfox/uboot/"

    # Platform-specific rootfs (MTD vs eMMC)
    "^ext_tree/board/luckfox/rootfs_overlay/etc/fstab$"
    "^ext_tree/board/luckfox/rootfs_overlay/etc/fw_env.config$"

    # Platform-specific init scripts (MTD vs eMMC)
    "^ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S00platform$"
    "^ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S20linkmount$"
    "^ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S94ioi2s$"
    "^ext_tree/board/luckfox/config/uboot-env.txt$"

    # Platform-specific helper scripts (DTB switching, update — different for MAX/Ultra)
    "^ext_tree/board/luckfox/rootfs_overlay/opt/2.*\.sh$"
    "^ext_tree/board/luckfox/rootfs_overlay/opt/export.sh$"
    "^ext_tree/board/luckfox/rootfs_overlay/opt/update.sh$"

    # Build output
    "^buildroot/output/"
)

echo "Step 1: Switching to $DST_BRANCH branch..."
git checkout "$DST_BRANCH"
ON_DESTINATION_BRANCH=1

SRC_HEAD=$(git rev-parse "$SRC_BRANCH")
DST_HEAD=$(git rev-parse "$DST_BRANCH")

echo "$SRC_BRANCH HEAD: $SRC_HEAD"
echo "$DST_BRANCH HEAD: $DST_HEAD"

echo "Step 2: Finding files that differ..."
mapfile -d '' CHANGED_FILES < <(git diff -z --name-only "$DST_BRANCH" "$SRC_BRANCH")

echo "Step 3: Filtering files (excluding Ultra-specific)..."
FILES_TO_SYNC=()
SKIPPED_COUNT=0
SYNCED_COUNT=0

for file in "${CHANGED_FILES[@]}"; do
    [ -z "$file" ] && continue

    SKIP=false
    for pattern in "${EXCLUDE_PATTERNS[@]}"; do
        if echo "$file" | grep -qE "$pattern"; then
            echo "  [SKIP] $file"
            SKIP=true
            SKIPPED_COUNT=$((SKIPPED_COUNT + 1))
            break
        fi
    done

    if [ "$SKIP" = false ]; then
        FILES_TO_SYNC+=("$file")
        SYNCED_COUNT=$((SYNCED_COUNT + 1))
    fi
done

echo ""
echo "Files to sync: $SYNCED_COUNT"
echo "Files skipped: $SKIPPED_COUNT"
echo ""

if [ ${#FILES_TO_SYNC[@]} -eq 0 ]; then
    echo "No files to sync!"
    git checkout "$SRC_BRANCH"
    ON_DESTINATION_BRANCH=0
    exit 0
fi

echo "Step 4: Syncing files from $SRC_BRANCH..."
for file in "${FILES_TO_SYNC[@]}"; do
    if git cat-file -e "$SRC_BRANCH":"$file" 2>/dev/null; then
        echo "  [SYNC] $file"
        mkdir -p "$(dirname "$file")"
        git restore --source "$SRC_BRANCH" --staged --worktree -- "$file"
    else
        echo "  [DELETE] $file (removed in $SRC_BRANCH)"
        git rm -f "$file" 2>/dev/null || rm -f "$file"
    fi
done

echo ""
echo "Step 5: Enabling new synced packages in the Ultra defconfig..."
MAX_DEFCONFIG="ext_tree/configs/luckfox_pico_max_defconfig"
ULTRA_DEFCONFIG="ext_tree/configs/luckfox_pico_ultra_defconfig"
PACKAGE_SYMBOLS=()

# Defconfigs are intentionally Ultra-specific. Only propagate explicitly
# enabled symbols for package Config.in files that did not exist in Ultra.
for file in "${FILES_TO_SYNC[@]}"; do
    case "$file" in
        ext_tree/package/*/Config.in)
            if ! git cat-file -e "$DST_HEAD:$file" 2>/dev/null; then
                while IFS= read -r symbol; do
                    PACKAGE_SYMBOLS+=("$symbol")
                done < <(git show "$SRC_BRANCH:$file" | \
                    sed -n 's/^config \(BR2_PACKAGE_[A-Z0-9_]*\)$/\1/p')
            fi
            ;;
    esac
done

if [ ${#PACKAGE_SYMBOLS[@]} -eq 0 ]; then
    echo "  No new package options to enable"
elif ! git cat-file -e "$SRC_BRANCH:$MAX_DEFCONFIG" 2>/dev/null || \
     [ ! -f "$ULTRA_DEFCONFIG" ]; then
    echo "  [WARN] Defconfig not found; package options were not updated"
else
    for symbol in "${PACKAGE_SYMBOLS[@]}"; do
        if git show "$SRC_BRANCH:$MAX_DEFCONFIG" | grep -qx "${symbol}=y"; then
            if grep -qx "${symbol}=y" "$ULTRA_DEFCONFIG"; then
                continue
            elif grep -qx "# ${symbol} is not set" "$ULTRA_DEFCONFIG"; then
                sed -i "s/^# ${symbol} is not set$/${symbol}=y/" "$ULTRA_DEFCONFIG"
            else
                echo "${symbol}=y" >> "$ULTRA_DEFCONFIG"
            fi
            git add "$ULTRA_DEFCONFIG"
            echo "  [ENABLE] ${symbol}"
        fi
    done
fi

echo ""
echo "Step 6: Updating branding (MAX → Ultra)..."
INDEX_PHP="ext_tree/board/luckfox/rootfs_overlay/var/www/index.php"
if [ -f "$INDEX_PHP" ]; then
    if grep -q "MAX" "$INDEX_PHP"; then
        sed -i 's/MAX/Ultra/g' "$INDEX_PHP"
        git add "$INDEX_PHP"
        echo "  [UPDATED] $INDEX_PHP (MAX → Ultra)"
    fi
fi

echo ""
echo "Step 7: Reviewing changes..."
git status

echo ""
echo "=== Sync complete! ==="
echo ""
echo "Review the changes with: git diff --cached"
echo "Commit with: git commit -m 'Sync from $SRC_BRANCH'"
echo "Discard with: git checkout $SRC_BRANCH && git checkout $DST_BRANCH -- . && git checkout $SRC_BRANCH"

SYNC_SUCCESS=1
