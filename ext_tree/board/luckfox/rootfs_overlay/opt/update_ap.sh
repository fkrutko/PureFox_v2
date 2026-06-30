#!/bin/sh
#
# update_ap.sh — обновление AP-продуктов (APlayer/APrender/APScream)
# напрямую с сайта автора albumplayer.ru
#
# Использование:
#   /opt/update_ap.sh aplayer      — обновить APlayer
#   /opt/update_ap.sh aprenderer   — обновить APrender
#   /opt/update_ap.sh apscream     — обновить APScream
#   /opt/update_ap.sh all          — обновить все три
#   /opt/update_ap.sh list         — показать версии

set -e

TMPDIR=/tmp/ap_update
LOG_TAG="update_ap"

# ── цвета ──────────────────────────────────────────────────────────
RED='\033[31m';    GRN='\033[32m';    YLW='\033[33m'
BLU='\033[34m';    MAG='\033[35m';    CYN='\033[36m'
BLD='\033[1m';     DIM='\033[2m';     RST='\033[0m'

# если вывод не в терминал (браузер, пайп) — убираем цвета
if [ ! -t 1 ]; then
    RED=''; GRN=''; YLW=''; BLU=''; MAG=''; CYN=''
    BLD=''; DIM=''; RST=''
fi

log()  { echo -e "${DIM}[${LOG_TAG}]${RST} $*"; logger -t "$LOG_TAG" "$*" 2>/dev/null || true; }
ok()   { log "${GRN}OK:${RST} $*"; }
warn() { log "${YLW}WARN:${RST} $*"; }
fail() { log "${RED}${BLD}ERROR:${RST} $*"; exit 1; }

# ── самообновление скрипта (как в update.sh) ──────────────────────
SCRIPT_PATH="/opt/update_ap.sh"
SCRIPT_NEW="/tmp/update_ap.sh.new"
UPDATE_SRC="luckfox@luckfox.puredsd.ru::luckfox2/opt/update_ap.sh"

if [ "$UPDATE_SELF_DONE" != "1" ]; then
    if command -v sshpass >/dev/null 2>&1 && \
       sshpass -p 'luckfox' rsync -aq --timeout=10 "$UPDATE_SRC" "$SCRIPT_NEW" 2>/dev/null && \
       [ -f "$SCRIPT_NEW" ] && [ -s "$SCRIPT_NEW" ]; then
        OLD_MD5=$(md5sum "$SCRIPT_PATH" 2>/dev/null | awk '{print $1}')
        NEW_MD5=$(md5sum "$SCRIPT_NEW" 2>/dev/null | awk '{print $1}')
        if [ "$OLD_MD5" != "$NEW_MD5" ]; then
            log "${YLW}New version of update_ap.sh found, updating...${RST}"
            chmod +x "$SCRIPT_NEW"
            cp "$SCRIPT_NEW" "$SCRIPT_PATH"
            rm -f "$SCRIPT_NEW"
            export UPDATE_SELF_DONE=1
            exec "$SCRIPT_PATH" "$@"
        fi
    fi
    rm -f "$SCRIPT_NEW"
fi

# ── параметры продуктов ───────────────────────────────────────────

APLAYER_URL="https://albumplayer.ru/linux/aplayer-arm32.tar.gz"
APRENDERER_URL="https://albumplayer.ru/linux/aprenderer-arm32.tar.gz"
APSCREAM_URL="https://albumplayer.ru/asioscream4.zip"
APSCREAM_INNER="asioscream4/LinuxReceiver/Arm32/apscream-arm32.tar.gz"

# ── утилиты ────────────────────────────────────────────────────────

download() {
    # $1=URL $2=outfile
    log "${CYN}Downloading $1 ..."
    if command -v wget >/dev/null 2>&1; then
        wget -T 120 -O "$2" "$1" || fail "wget failed for $1"
    elif command -v curl >/dev/null 2>&1; then
        curl -L --connect-timeout 60 -o "$2" "$1" || fail "curl failed for $1"
    else
        fail "Neither wget nor curl found"
    fi
}

# BusyBox tar не поддерживает -z/-a — распаковка через gzip|tar
extract_tgz() {
    # $1=tarball $2=destdir
    gzip -dc "$1" | tar -x -C "$2" || fail "Extract failed"
}

is_running() {
    pidof "$1" >/dev/null 2>&1
}

backup_data() {
    # $1=target_dir $2=pattern (e.g. "*.dat")
    local dir="$1" pat="$2" bak="$TMPDIR/data_backup"
    mkdir -p "$bak"
    if [ -d "$dir" ]; then
        find "$dir" -maxdepth 1 -name "$pat" -exec cp -a {} "$bak/" \; 2>/dev/null || true
        log "Backed up $(ls "$bak" 2>/dev/null | wc -l) data files from $dir"
    fi
}

restore_data() {
    local dir="$1" bak="$TMPDIR/data_backup"
    if [ -d "$bak" ] && [ "$(ls -A "$bak" 2>/dev/null)" ]; then
        cp -a "$bak"/* "$dir/" 2>/dev/null || true
        log "Restored data files to $dir"
    fi
}

# ── патчи веб-интерфейса (как в .mk при сборке) ─────────────────

patch_aplayer_webui() {
    log "${MAG}Patching APlayer web UI..."
    local HTML="/usr/aplayer/aplayer.html"
    local DHTML="/usr/aplayer/dimas/aplayer.html"

    for f in "$HTML" "$DHTML"; do
        [ ! -f "$f" ] && continue
        sed -i 's|id="check_preload" type="checkbox"> Full preloading|id="check_preload" type="checkbox" disabled> Full preloading|' "$f"
        sed -i 's|id="mmap" name="mmap" type="radio" checked|id="mmap" name="mmap" type="radio" disabled|' "$f"
        sed -i 's|id="rw" name="mmap" type="radio"|id="rw" name="mmap" type="radio" checked|' "$f"
        sed -i 's|id="check_memory" type="checkbox" checked>|id="check_memory" type="checkbox" disabled>|' "$f"
        sed -i 's|id="cores0" type="radio" name="cores" checked|id="cores0" type="radio" name="cores" disabled|' "$f"
        sed -i 's|id="cores2" type="radio" name="cores"|id="cores2" type="radio" name="cores" disabled|' "$f"
        sed -i 's|id="CardNum" style="width:24px"|id="CardNum" style="width:24px" disabled|' "$f"
    done

    # API поля — только в aplayer.html
    if [ -f "$HTML" ]; then
        sed -i '/id="radio_pict"/s|(1-10)</td></tr>|(1-10)  \&nbsp;\&nbsp; API cx \&nbsp;<input id="api_cx" style="width:150px" type="text"></td></tr>\n        <tr><td>API key \&nbsp; <input id="api_key" style="width:340px" type="text"></td></tr>|' "$HTML"
    fi
    log "${GRN}APlayer web UI patched${RST}"
}

patch_aprenderer_webui() {
    log "${MAG}Patching APrender web UI..."
    local f="/usr/aprenderer/renderer.html"
    [ ! -f "$f" ] && return

    sed -i 's|id="check_preload" type="checkbox"> Full preloading|id="check_preload" type="checkbox" disabled> Full preloading|' "$f"
    sed -i 's|id="mmap" name="mmap" type="radio" checked|id="mmap" name="mmap" type="radio" disabled|' "$f"
    sed -i 's|id="rw" name="mmap" type="radio"|id="rw" name="mmap" type="radio" checked|' "$f"
    sed -i 's|id="check_memory" type="checkbox" checked>|id="check_memory" type="checkbox" disabled>|' "$f"
    sed -i 's|id="cores0" type="radio" name="cores" checked|id="cores0" type="radio" name="cores" disabled|' "$f"
    sed -i 's|id="cores2" type="radio" name="cores"|id="cores2" type="radio" name="cores" disabled|' "$f"
    sed -i 's|id="CardNum" style="width:24px"|id="CardNum" style="width:24px" disabled|' "$f"
    log "${GRN}APrender web UI patched${RST}"
}

# ── обновление APlayer ─────────────────────────────────────────────

update_aplayer() {
    log "${CYN}${BLD}=== Updating APlayer ===${RST}"
    local WAS_RUNNING=false
    is_running aplayer && WAS_RUNNING=true

    if $WAS_RUNNING; then
        log "${DIM}Stopping APlayer..."
        /etc/rc.pure/S95aplayer stop 2>/dev/null || true
        killall -9 aplayer 2>/dev/null || true
        sleep 1
    fi

    mkdir -p "$TMPDIR"
    backup_data /usr/aplayer "*.dat"

    download "$APLAYER_URL" "$TMPDIR/aplayer.tar.gz"

    # удаляем старый пакет (но не .dat)
    if [ -d /usr/aplayer ]; then
        find /usr/aplayer -mindepth 1 ! -name '*.dat' -exec rm -rf {} + 2>/dev/null || true
    fi

    log "${YLW}Extracting...${RST}"
    extract_tgz "$TMPDIR/aplayer.tar.gz" /usr/
    restore_data /usr/aplayer
    patch_aplayer_webui
    sync

    if $WAS_RUNNING; then
        log "${BLU}Restarting APlayer..."
        /etc/rc.pure/S95aplayer start 2>/dev/null || true
        sleep 1
        is_running aplayer && ok "APlayer started" || warn "APlayer did not start"
    fi

    log "${GRN}${BLD}APlayer updated successfully${RST}"
}

# ── обновление APrender ────────────────────────────────────────────

update_aprenderer() {
    log "${CYN}${BLD}=== Updating APrender ===${RST}"
    local WAS_RUNNING=false
    is_running ap2renderer && WAS_RUNNING=true

    if $WAS_RUNNING; then
        log "${DIM}Stopping APrender..."
        /etc/rc.pure/S95aprenderer stop 2>/dev/null || true
        killall -9 ap2renderer 2>/dev/null || true
        sleep 1
    fi

    mkdir -p "$TMPDIR"
    backup_data /usr/aprenderer "*.dat"

    download "$APRENDERER_URL" "$TMPDIR/aprenderer.tar.gz"

    if [ -d /usr/aprenderer ]; then
        find /usr/aprenderer -mindepth 1 ! -name '*.dat' -exec rm -rf {} + 2>/dev/null || true
    fi

    log "${YLW}Extracting...${RST}"
    extract_tgz "$TMPDIR/aprenderer.tar.gz" /usr/
    restore_data /usr/aprenderer
    patch_aprenderer_webui
    sync

    if $WAS_RUNNING; then
        log "${BLU}Restarting APrender..."
        /etc/rc.pure/S95aprenderer start 2>/dev/null || true
        sleep 1
        is_running ap2renderer && ok "APrender started" || warn "APrender did not start"
    fi

    log "${GRN}${BLD}APrender updated successfully${RST}"
}

# ── обновление APScream ────────────────────────────────────────────

update_apscream() {
    log "${CYN}${BLD}=== Updating APScream ===${RST}"
    local WAS_RUNNING=false
    is_running apscream && WAS_RUNNING=true

    if $WAS_RUNNING; then
        log "${DIM}Stopping APScream..."
        /etc/rc.pure/S95apscream stop 2>/dev/null || true
        killall -9 apscream 2>/dev/null || true
        sleep 1
    fi

    mkdir -p "$TMPDIR"
    # бэкап config.txt
    if [ -f /usr/apscream/config.txt ]; then
        cp -a /usr/apscream/config.txt "$TMPDIR/apscream_config.txt.bak"
        log "Backed up config.txt"
    fi

    download "$APSCREAM_URL" "$TMPDIR/apscream.zip"

    log "${YLW}Extracting inner archive from zip..."
    unzip -o -j "$TMPDIR/apscream.zip" "$APSCREAM_INNER" -d "$TMPDIR/" \
        || fail "Failed to extract $APSCREAM_INNER from zip"

    log "${YLW}Extracting apscream..."
    mkdir -p /usr/apscream "$TMPDIR/apscream_extract"
    extract_tgz "$TMPDIR/apscream-arm32.tar.gz" "$TMPDIR/apscream_extract/"

    # копируем только новые файлы, сохраняя config.txt
    if [ -f "$TMPDIR/apscream_config.txt.bak" ]; then
        cp -a "$TMPDIR/apscream_extract/"* /usr/apscream/ 2>/dev/null || true
        cp -a "$TMPDIR/apscream_config.txt.bak" /usr/apscream/config.txt
        log "Restored config.txt"
    else
        cp -a "$TMPDIR/apscream_extract/"* /usr/apscream/ 2>/dev/null || true
    fi

    chmod +x /usr/apscream/apscream 2>/dev/null || true
    sync

    if $WAS_RUNNING; then
        log "${BLU}Restarting APScream..."
        /etc/rc.pure/S95apscream start 2>/dev/null || true
        sleep 1
        is_running apscream && ok "APScream started" || warn "APScream did not start"
    fi

    log "${GRN}${BLD}APScream updated successfully${RST}"
}

# ── список версий ──────────────────────────────────────────────────

list_versions() {
    # шапка
    printf "  ${BLD}%-14s %8s %8s %s${RST}\n" "Name" "Version" "Size" "Status"
    printf "  -------------- -------- -------- -------------\n"

    for d in /usr/aplayer /usr/aprenderer /usr/apscream; do
        if [ -d "$d" ]; then
            name=$(basename "$d")
            bin=""
            case "$name" in
                aplayer)    bin="$d/aplayer"; proc="aplayer" ;;
                aprenderer) bin="$d/ap2renderer"; proc="ap2renderer" ;;
                apscream)   bin="$d/apscream"; proc="apscream" ;;
            esac
            if [ -x "$bin" ]; then
                ver=$(grep -aoE "(Album Player|APlayer).*(for Linux|Receiver) [0-9.]+" "$bin" 2>/dev/null | head -1 | grep -oE '[0-9.]+$')
                [ -z "$ver" ] && ver="?"
                size=$(ls -lh "$bin" 2>/dev/null | awk '{print $5}')
                [ -z "$size" ] && size="?"

                if pidof "$proc" >/dev/null 2>&1; then
                    status="${GRN}${BLD}RUNNING${RST}"
                else
                    status="${RED}stopped${RST}"
                fi

                printf "  ${GRN}%-14s${RST} ${CYN}%8s${RST} ${YLW}%8s${RST} %b\n" \
                    "$name" "v$ver" "$size" "$status"
            else
                printf "  ${DIM}%-14s -- not installed --${RST}\n" "$name"
            fi
        fi
    done
}

# ── главная логика ─────────────────────────────────────────────────

usage() {
    echo "Usage: $0 {aplayer|aprenderer|apscream|all|list}"
    echo ""
    echo "  aplayer     — обновить APlayer с albumplayer.ru"
    echo "  aprenderer  — обновить APrender с albumplayer.ru"
    echo "  apscream    — обновить APScream с albumplayer.ru"
    echo "  all         — обновить все три продукта"
    echo "  list        — показать установленные версии"
    exit 1
}

[ $# -eq 0 ] && usage

mkdir -p "$TMPDIR"

case "$1" in
    aplayer)
        update_aplayer
        ;;
    aprenderer)
        update_aprenderer
        ;;
    apscream)
        update_apscream
        ;;
    all)
        update_aplayer
        update_aprenderer
        update_apscream
        log "${GRN}${BLD}=== All AP products updated ===${RST}"
        ;;
    list)
        list_versions
        exit 0
        ;;
    *)
        usage
        ;;
esac

# очистка
rm -rf "$TMPDIR"
log "${GRN}Done.${RST}"
