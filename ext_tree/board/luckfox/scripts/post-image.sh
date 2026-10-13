#!/bin/sh

set -eu

BINARIES_DIR=${1:?Usage: $0 <binaries-dir>}
ENV_IMAGE="$BINARIES_DIR/uboot-env.bin"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ENV_SOURCE="$SCRIPT_DIR/../config/uboot-env.txt"
ENV_SIZE=0x40000

[ -f "$BINARIES_DIR/rootfs.ubi" ] || {
    echo "Missing required release artifact: $BINARIES_DIR/rootfs.ubi" >&2
    exit 1
}

regenerate_environment_image() {
    temporary_image="${ENV_IMAGE}.tmp.$$"

    [ -r "$ENV_SOURCE" ] || {
        echo "U-Boot environment source not found: $ENV_SOURCE" >&2
        return 1
    }
    [ -n "${HOST_DIR:-}" ] || {
        echo "Buildroot did not provide HOST_DIR for U-Boot environment generation" >&2
        return 1
    }
    ENV_TOOL="$HOST_DIR/bin/mkenvimage"
    [ -x "$ENV_TOOL" ] || {
        echo "U-Boot environment generator not found: $ENV_TOOL" >&2
        return 1
    }

    rm -f "$temporary_image"
    if ! "$ENV_TOOL" -s "$ENV_SIZE" -o "$temporary_image" - < "$ENV_SOURCE"; then
        rm -f "$temporary_image"
        return 1
    fi
    mv -f "$temporary_image" "$ENV_IMAGE"
}

if [ ! -f "$ENV_IMAGE" ]; then
    regenerate_environment_image || {
        echo "Missing required release artifact: $ENV_IMAGE" >&2
        exit 1
    }
fi

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
publish_release_artifact "$ENV_IMAGE" "$BINARIES_DIR/env.img"

cleanup_release_intermediates() {
    for artifact in rootfs.img env.img; do
        [ -f "$BINARIES_DIR/$artifact" ] || {
            echo "Refusing to prune artifacts: missing $BINARIES_DIR/$artifact" >&2
            return 1
        }
    done

    for artifact in rootfs.ubi rootfs.ubifs uboot-env.bin; do
        rm -f "$BINARIES_DIR/$artifact"
    done
    rm -f "$BINARIES_DIR"/*.dtb
}

cleanup_release_intermediates
