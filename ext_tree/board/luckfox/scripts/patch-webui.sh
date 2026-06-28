#!/bin/bash
# Применяет PureFox-специфичные блокировки к веб-интерфейсам aplayer/aprenderer
# Использование: patch-webui.sh <архив_aplayer.tar.gz> <архив_aprenderer.tar.gz> <версия>
# Пример: patch-webui.sh dl/aplayer/aplayer-arm32.tar.gz dl/aprenderer/aprenderer-arm32.tar.gz 2.18

set -e

APLAYER_ARCHIVE="${1:?Укажи путь к архиву aplayer}"
APRENDERER_ARCHIVE="${2:?Укажи путь к архиву aprenderer}"
VERSION="${3:?Укажи версию, например 2.18}"

OVERLAY="${OVERLAY:-/mnt/sdb/PureFox_v2/ext_tree/board/luckfox/rootfs_overlay}"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "=== Извлекаю архивы ==="
mkdir -p "$TMPDIR/aplayer" "$TMPDIR/aprenderer"
tar xzf "$APLAYER_ARCHIVE"   -C "$TMPDIR/aplayer"   --strip-components=1
tar xzf "$APRENDERER_ARCHIVE" -C "$TMPDIR/aprenderer" --strip-components=1

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
apply_common "$TMPDIR/aprenderer/renderer.html" "APlayer Media Renderer"
cp "$TMPDIR/aprenderer/renderer.html" "$OVERLAY/usr/aprenderer/renderer.html"

# ===================================================================
# aplayer.html (+ API поля)
# ===================================================================
echo "=== aplayer.html ==="
F="$TMPDIR/aplayer/aplayer.html"
apply_common "$F" "Album Player"

# Добавить API поля после строки с radio_pict
sed -i '/id="radio_pict"/{
s|(1-10)</td></tr>|(1-10)  \&nbsp;\&nbsp; API cx \&nbsp;<input id="api_cx" style="width:150px" type="text"></td></tr>|
a\        <tr><td>API key \&nbsp; <input id="api_key" style="width:340px" type="text"></td></tr>
}' "$F"

cp "$F" "$OVERLAY/usr/aplayer/aplayer.html"

# ===================================================================
# dimas/aplayer.html
# ===================================================================
echo "=== dimas/aplayer.html ==="
F="$TMPDIR/aplayer/dimas/aplayer.html"

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

cp "$F" "$OVERLAY/usr/aplayer/dimas/aplayer.html"

echo "=== Готово ==="
echo "Версия $VERSION применена. Файлы в $OVERLAY/usr/"
ls -la "$OVERLAY/usr/aplayer/aplayer.html" "$OVERLAY/usr/aplayer/dimas/aplayer.html" "$OVERLAY/usr/aprenderer/renderer.html"
