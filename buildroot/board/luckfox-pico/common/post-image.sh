#!/bin/sh

set -eu

BINARIES_DIR=${1:?Usage: $0 <binaries-dir>}

for artifact in rootfs.ubi uboot-env.bin; do
    [ -f "$BINARIES_DIR/$artifact" ] || {
        echo "Missing required release artifact: $BINARIES_DIR/$artifact" >&2
        exit 1
    }
done

publish_release_artifact() {
    source_file=$1
    target_file=$2
    temporary_file="${target_file}.tmp.$$"

    rm -f "$temporary_file"
    if ! cp "$source_file" "$temporary_file"; then
        rm -f "$temporary_file"
        return 1
    fi
    mv -f "$temporary_file" "$target_file"
}

publish_release_artifact "$BINARIES_DIR/rootfs.ubi" "$BINARIES_DIR/rootfs.img"
publish_release_artifact "$BINARIES_DIR/uboot-env.bin" "$BINARIES_DIR/env.img"
