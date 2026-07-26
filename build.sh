#!/bin/sh

set -eu

usage() {
    echo "Usage: $0 [--bootstrap] [--clean]" >&2
}

CLEAN_BUILD=0
BOOTSTRAP=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --clean) CLEAN_BUILD=1 ;;
        --bootstrap) BOOTSTRAP=1 ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 1
            ;;
    esac
    shift
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILDROOT_DIR="$SCRIPT_DIR/buildroot"
EXTERNAL_DIR="$SCRIPT_DIR/ext_tree"
OUTPUT_DIR=${O:-"$BUILDROOT_DIR/output"}

case "$OUTPUT_DIR" in
    /*) ;;
    *) OUTPUT_DIR="$SCRIPT_DIR/$OUTPUT_DIR" ;;
esac

[ -d "$BUILDROOT_DIR" ] || {
    echo "Buildroot directory not found: $BUILDROOT_DIR" >&2
    exit 1
}
[ -f "$EXTERNAL_DIR/external.desc" ] || {
    echo "PureFox external tree not found: $EXTERNAL_DIR" >&2
    exit 1
}

# Buildroot rejects a PATH containing whitespace or embedded newlines. Keep
# the build environment deterministic instead of inheriting shell startup noise.
BUILDROOT_PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
PATH="$BUILDROOT_PATH"
export PATH

if [ "$BOOTSTRAP" -eq 1 ]; then
    apt-get install -y git rsync build-essential cmake device-tree-compiler bc binutils libncurses-dev clang file
fi

for tool in git rsync make cmake dtc bc nm file; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required host tool: $tool (run $0 --bootstrap)" >&2
        exit 1
    }
done

LIBCLANG_PATH=${LIBCLANG_PATH:-/usr/lib/llvm-14/lib}
CLANG_PATH=${CLANG_PATH:-/usr/bin/clang}
command -v "$CLANG_PATH" >/dev/null 2>&1 || {
    echo "Clang not found: $CLANG_PATH" >&2
    exit 1
}
[ -d "$LIBCLANG_PATH" ] || {
    echo "libclang directory not found: $LIBCLANG_PATH" >&2
    exit 1
}

cd "$BUILDROOT_DIR"
if [ "$CLEAN_BUILD" -eq 1 ] && [ -f "$OUTPUT_DIR/.config" ]; then
    make O="$OUTPUT_DIR" clean
fi

make O="$OUTPUT_DIR" BR2_EXTERNAL="$EXTERNAL_DIR" luckfox_pico_max_defconfig

if [ ! -f "$OUTPUT_DIR/images/uboot-env.bin" ]; then
    make O="$OUTPUT_DIR" host-uboot-tools-rebuild
fi

# Generic local packages are copied into Buildroot's build tree only once.
# Rebuild these audio binaries so a normal ./build.sh always includes source
# edits made under ext_tree/package/.
for rebuild_target in status-monitor-rebuild volume-encoder-rebuild; do
    make O="$OUTPUT_DIR" "$rebuild_target"
done

export FORCE_UNSAFE_CONFIGURE=1
export LIBCLANG_PATH
export CLANG_PATH
export BINDGEN_EXTRA_CLANG_ARGS="--sysroot=$OUTPUT_DIR/host/arm-buildroot-linux-gnueabihf/sysroot"

make O="$OUTPUT_DIR"