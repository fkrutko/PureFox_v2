#!/bin/bash
# Применяет PureFox-специфичные блокировки к веб-интерфейсам aplayer/aprenderer
# Использование: patch-webui.sh <архив_aplayer.tar.gz> <архив_aprenderer.tar.gz> <версия>
# Пример: patch-webui.sh dl/aplayer/aplayer-arm32.tar.gz dl/aprenderer/aprenderer-arm32.tar.gz 2.18

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

APLAYER_ARCHIVE="${1:?Укажи путь к архиву aplayer}"
APRENDERER_ARCHIVE="${2:?Укажи путь к архиву aprenderer}"
VERSION="${3:?Укажи версию, например 2.18}"

OVERLAY="${OVERLAY:-$SCRIPT_DIR/../rootfs_overlay}"
TMPDIR=$(mktemp -d)
PUBLISH_PENDING=0
BACKUP_DIR="$TMPDIR/backup"
STAGE_DIR="$TMPDIR/publish"
RENDERER_SOURCE="$TMPDIR/aprenderer/renderer.html"
APLAYER_SOURCE="$TMPDIR/aplayer/aplayer.html"
DIMAS_SOURCE="$TMPDIR/aplayer/dimas/aplayer.html"
RENDERER_TARGET="$OVERLAY/usr/aprenderer/renderer.html"
APLAYER_TARGET="$OVERLAY/usr/aplayer/aplayer.html"
DIMAS_TARGET="$OVERLAY/usr/aplayer/dimas/aplayer.html"

rollback_publish() {
    [ "$PUBLISH_PENDING" -eq 1 ] || return 0
    echo "=== Откат публикации WebUI ===" >&2
    cp "$BACKUP_DIR/renderer.html" "$RENDERER_TARGET"
    cp "$BACKUP_DIR/aplayer.html" "$APLAYER_TARGET"
    cp "$BACKUP_DIR/dimas-aplayer.html" "$DIMAS_TARGET"
}

cleanup() {
    status=$?
    trap - EXIT
    rollback_publish
    rm -rf "$TMPDIR"
    exit "$status"
}
trap cleanup EXIT

validate_input_pages() {
    for page in "$RENDERER_SOURCE" "$APLAYER_SOURCE" "$DIMAS_SOURCE"; do
        [ -f "$page" ] || {
            echo "Missing expected WebUI page: $page" >&2
            exit 1
        }
    done
    for page in "$RENDERER_TARGET" "$APLAYER_TARGET" "$DIMAS_TARGET"; do
        [ -f "$page" ] || {
            echo "Missing overlay WebUI page: $page" >&2
            exit 1
        }
    done
}

validate_patched_pages() {
    grep -Fq "<title>APlayer Media Renderer ${VERSION}</title>" "$RENDERER_SOURCE"
    grep -Fq "<title>Album Player ${VERSION}</title>" "$APLAYER_SOURCE"
    grep -Fq "<title>Album Player ${VERSION}</title>" "$DIMAS_SOURCE"
    grep -Fq 'id="api_cx"' "$APLAYER_SOURCE"
    grep -Fq 'id="api_key"' "$APLAYER_SOURCE"
}

stage_and_publish() {
    mkdir -p "$BACKUP_DIR" "$STAGE_DIR"
    cp "$RENDERER_TARGET" "$BACKUP_DIR/renderer.html"
    cp "$APLAYER_TARGET" "$BACKUP_DIR/aplayer.html"
    cp "$DIMAS_TARGET" "$BACKUP_DIR/dimas-aplayer.html"

    cp "$RENDERER_SOURCE" "$STAGE_DIR/renderer.html"
    cp "$APLAYER_SOURCE" "$STAGE_DIR/aplayer.html"
    cp "$DIMAS_SOURCE" "$STAGE_DIR/dimas-aplayer.html"

    PUBLISH_PENDING=1
    mv "$STAGE_DIR/renderer.html" "$RENDERER_TARGET"
    mv "$STAGE_DIR/aplayer.html" "$APLAYER_TARGET"
    mv "$STAGE_DIR/dimas-aplayer.html" "$DIMAS_TARGET"
    PUBLISH_PENDING=0
}

echo "=== Извлекаю архивы ==="
mkdir -p "$TMPDIR/aplayer" "$TMPDIR/aprenderer"
tar xzf "$APLAYER_ARCHIVE"   -C "$TMPDIR/aplayer"   --strip-components=1
tar xzf "$APRENDERER_ARCHIVE" -C "$TMPDIR/aprenderer" --strip-components=1
validate_input_pages

# ===================================================================
# Общие блокировки для aplayer.html и renderer.html
# ===================================================================
apply_common() {
    local F="$1"
    local TITLE="$2"

    # Версия в title
    sed -i "s|<title>${TITLE} [0-9.]*</title>|<title>${TITLE} ${VERSION}</title>|" "$F"

    # Full Memory + Full preloading -> disabled
    sed -i 's|id="radio_fm" type="radio" name="pm"|id="radio_fm" type="radio" name="pm" disabled|' "$F"
    sed -i 's|id="check_preload" type="checkbox"> Full preloading|id="check_preload" type="checkbox" disabled> Full preloading|' "$F"

    # MMAP -> disabled, RW -> checked
    sed -i 's|id="mmap" name="mmap" type="radio" checked|id="mmap" name="mmap" type="radio" disabled|' "$F"
    sed -i 's|id="rw" name="mmap" type="radio"|id="rw" name="mmap" type="radio" checked|' "$F"

    # Lock memory -> disabled
    sed -i 's|id="check_memory" type="checkbox" checked>|id="check_memory" type="checkbox" disabled>|' "$F"

    # Выбор ядер -> disabled
    sed -i 's|id="cores0" type="radio" name="cores" checked|id="cores0" type="radio" name="cores" disabled|' "$F"
    sed -i 's|id="cores2" type="radio" name="cores"|id="cores2" type="radio" name="cores" disabled|' "$F"

    # CardNum -> disabled
    sed -i 's|id="CardNum" style="width:24px"|id="CardNum" style="width:24px" disabled|' "$F"
}

# ===================================================================
# renderer.html
# ===================================================================
echo "=== renderer.html ==="
apply_common "$RENDERER_SOURCE" "APlayer Media Renderer"

# ===================================================================
# aplayer.html (+ API поля)
# ===================================================================
echo "=== aplayer.html ==="
F="$APLAYER_SOURCE"
apply_common "$F" "Album Player"

# Добавить API поля после строки с radio_pict
sed -i '/id="radio_pict"/{
s|(1-10)</td></tr>|(1-10)  \&nbsp;\&nbsp; API cx \&nbsp;<input id="api_cx" style="width:150px" type="text"></td></tr>|
a\        <tr><td>API key \&nbsp; <input id="api_key" style="width:340px" type="text"></td></tr>
}' "$F"

# ===================================================================
# dimas/aplayer.html
# ===================================================================
echo "=== dimas/aplayer.html ==="
F="$DIMAS_SOURCE"

# Версия
sed -i "s|<title>Album Player [0-9.]*</title>|<title>Album Player ${VERSION}</title>|" "$F"

# Блокировки (dimas использует другие классы)
sed -i 's|id="radio_fm" type="radio" name="pm"|id="radio_fm" type="radio" name="pm" disabled|' "$F"
sed -i 's|id="check_preload" type="checkbox"|id="check_preload" type="checkbox" disabled|' "$F"
sed -i 's|id="mmap" name="mmap" type="radio" checked|id="mmap" name="mmap" type="radio" disabled|' "$F"
sed -i 's|id="rw" name="mmap" type="radio"|id="rw" name="mmap" type="radio" checked|' "$F"
sed -i 's|id="check_memory" type="checkbox" checked|id="check_memory" type="checkbox" disabled|' "$F"
sed -i 's|id="cores0" type="radio" name="cores" checked|id="cores0" type="radio" name="cores" disabled|' "$F"
sed -i 's|id="cores2" type="radio" name="cores"|id="cores2" type="radio" name="cores" disabled|' "$F"
sed -i 's|id="CardNum" style="width:24px"|id="CardNum" style="width:24px" disabled|' "$F"

validate_patched_pages
stage_and_publish

echo "=== Готово ==="
echo "Версия $VERSION применена. Файлы в $OVERLAY/usr/"
ls -la "$OVERLAY/usr/aplayer/aplayer.html" "$OVERLAY/usr/aplayer/dimas/aplayer.html" "$OVERLAY/usr/aprenderer/renderer.html"
