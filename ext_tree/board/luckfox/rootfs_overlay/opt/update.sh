#!/bin/sh
#
# PureFox online update from GitHub Releases (release "latest").
#
#   /opt/update.sh           update if a different build is published
#   /opt/update.sh --force   reinstall even if the build is the same
#   /opt/update.sh --check   only print the installed and the latest build
#
# How it works:
#   1. download version.txt, SHA256SUMS, rootfs.squashfs and boot.img to /tmp (RAM)
#      over HTTPS with certificate verification (curl + ca-certificates)
#   2. check SHA-256
#   3. loop-mount rootfs.squashfs and rsync it over / (each file is replaced
#      atomically by rename; user settings are kept, see KEEP_* below)
#   4. flash the selected kernel only if it changed (6.1: the kernel part of
#      boot.img; 5.10 or 1.6, see /opt/kernel.sh: the new
#      /data/boot/<kernel>/zImage),
#      then put back the DTB of that kernel for the current I2S mode
#      (PLL / EXT 1024 / EXT 512)
#   5. reboot
# idblock, U-Boot and the U-Boot environment are never touched online:
# for those flash the full image with RV1106_Toolkit.

REPO="fkrutko/PureFox_v2"
BASE="https://github.com/$REPO/releases/latest/download"
WORK=/tmp/purefox-update
NEW=$WORK/root
LOCK=/tmp/purefox-update.lock
BOOT_MTD=/dev/mtd3
DTB_OFFSET=3932160          # 0x3C0000, as in post-build.sh and /opt/2*.sh
MIN_RAM_MB=90               # rootfs.squashfs + boot.img + headroom
CUR_BUILD=$(cat /etc/purefox-release 2>/dev/null || echo unknown)

# Files the user changes at runtime. Kept as they are on the device when they
# exist there; a file missing on the device is installed from the new image.
KEEP_IF_PRESENT="
/etc/asound.conf
/etc/output
/etc/usb_to_i2s.state
/etc/dlna_bridge.conf
/etc/dlna_bridge.state
/etc/shadow
/etc/resolv.conf
/data/ethaddr.txt
/etc/scream_receiver.conf
/etc/kernel
"
# Created on the device only, never shipped in the image.
KEEP_PATTERNS="
/etc/init.d/S95*
/etc/dropbear/dropbear_*_host_key
"

log()  { echo "$*"; }
fail() { echo "ERROR: $*"; cleanup; exit 1; }

cleanup() {
    grep -q " $NEW " /proc/mounts 2>/dev/null && umount "$NEW"
    rm -rf "$WORK"
    rmdir "$LOCK" 2>/dev/null
}

fetch() {   # fetch <name> <out>
    if [ -z "$HAVE_CURL" ]; then
        # firmware built before curl was added: BusyBox wget does HTTPS but
        # does NOT verify the certificate; the image after this update has curl
        wget -q -T 60 -O "$2" "$BASE/$1"
        return
    fi
    curl -fsSL --retry 3 --connect-timeout 20 --max-time 900 -o "$2" "$BASE/$1"
    rc=$?
    # 60: certificate problem — usually the clock is not set yet (no NTP)
    [ $rc -eq 60 ] && log "TLS certificate check failed. Device time: $(date). Check network time (chrony)."
    return $rc
}

HAVE_CURL=$(command -v curl)
[ -n "$HAVE_CURL" ] || log "WARNING: curl not found, using BusyBox wget (server certificate not verified)."

# merge_conf <saved-old> <new>: keep the user's values for every KEY= the new file still has
merge_conf() {
    while IFS='=' read -r key val || [ -n "$key" ]; do   # last line may lack a newline
        case "$key" in ''|\#*|*[!A-Za-z0-9_]*) continue ;; esac
        case "$val" in *[\|\&\\]*) continue ;; esac
        grep -q "^$key=" "$2" && sed -i "s|^$key=.*|$key=$val|" "$2"
    done < "$1"
}

FORCE=0
case "$1" in
    --force) FORCE=1 ;;
    --check)
        log "Installed: $CUR_BUILD"
        log "Latest:    $(fetch version.txt - 2>/dev/null || echo 'GitHub not reachable')"
        exit 0 ;;
esac

mkdir "$LOCK" 2>/dev/null || { echo "ERROR: another update is already running"; exit 1; }
trap 'cleanup; exit 1' INT TERM

rm -rf "$WORK"; mkdir -p "$NEW" || fail "cannot create $WORK"

log "Installed build: $CUR_BUILD"
log "Checking GitHub ($REPO)..."
fetch version.txt "$WORK/version.txt" || fail "GitHub not reachable"
NEW_BUILD=$(tr -d '\r\n' < "$WORK/version.txt")
log "Latest build:    $NEW_BUILD"
if [ "$NEW_BUILD" = "$CUR_BUILD" ] && [ $FORCE -eq 0 ]; then
    log "Already up to date."
    cleanup; exit 0
fi

fetch SHA256SUMS "$WORK/SHA256SUMS" || fail "SHA256SUMS download failed"
for f in rootfs.squashfs boot.img; do
    grep -q " $f\$" "$WORK/SHA256SUMS" || fail "release has no checksum for $f"
done

avail=$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo)
log "Free memory: ${avail} MB"
[ "${avail:-0}" -ge $MIN_RAM_MB ] || fail "not enough free memory (need ~$MIN_RAM_MB MB), reboot and try again"

log "Downloading the image..."
fetch rootfs.squashfs "$WORK/rootfs.squashfs" || fail "rootfs.squashfs download failed"
fetch boot.img "$WORK/boot.img" || fail "boot.img download failed"
log "Verifying checksums..."
( cd "$WORK" && grep -E ' (rootfs\.squashfs|boot\.img)$' SHA256SUMS | sha256sum -c - ) || fail "checksum mismatch"

mount -t squashfs -o loop,ro "$WORK/rootfs.squashfs" "$NEW" || fail "cannot mount the image"
[ -f "$NEW/etc/purefox-release" ] && [ -f "$NEW/var/www/config.php" ] && [ -d "$NEW/data/boot" ] \
    || fail "image does not look like PureFox"

# ---- stop players, save I2S and low noise settings ----
log "Stopping players..."
for s in /etc/init.d/S95*; do [ -x "$s" ] && "$s" stop >/dev/null 2>&1; done
killall status_monitor 2>/dev/null
cp /etc/i2s.conf "$WORK/i2s.conf" 2>/dev/null
cp /etc/lownoise.conf "$WORK/lownoise.conf" 2>/dev/null

# ---- files ----
set --
for p in $KEEP_IF_PRESENT; do
    { [ -e "$p" ] || [ -L "$p" ]; } && set -- "$@" "--exclude=$p"
done
set -f      # patterns go to rsync as they are
for p in $KEEP_PATTERNS; do
    set -- "$@" "--exclude=$p"
done
set +f

log "Updating system files..."
rsync -acH --numeric-ids --delete-before \
    --exclude=/dev --exclude=/proc --exclude=/sys --exclude=/tmp --exclude=/run \
    --exclude=/mnt --exclude=/media --exclude=/root --exclude=/var/run --exclude=/var/log \
    --filter='protect /var/lib/***' \
    --filter='protect /var/cache/***' \
    "$@" \
    "$NEW/" / || fail "rsync failed: the system may be partly updated, run the update again"

# new i2s.conf may carry new keys: take it and put back the user's values
[ -f "$WORK/i2s.conf" ] && merge_conf "$WORK/i2s.conf" /etc/i2s.conf
[ -f "$WORK/lownoise.conf" ] && merge_conf "$WORK/lownoise.conf" /etc/lownoise.conf
MODE=$(sed -n 's/^MODE=//p' /etc/i2s.conf 2>/dev/null)
MCLK=$(sed -n 's/^MCLK=//p' /etc/i2s.conf 2>/dev/null)

umount "$NEW"
sync

# ---- kernel ----
KERNEL=$(cat /etc/kernel 2>/dev/null)
case "$KERNEL" in 5.10|1.6) ;; *) KERNEL=6.1 ;; esac
if [ "$KERNEL" != 6.1 ] && [ ! -f "/data/boot/$KERNEL/zImage" ]; then
    log "This release has no kernel $KERNEL, switching to 6.1."
    rm -f /etc/kernel
    KERNEL=6.1
fi
if [ "$KERNEL" != 6.1 ]; then
    KDIR=/data/boot/$KERNEL
    size=$(wc -c < "$KDIR/zImage")
    dd if="$BOOT_MTD" bs=65536 count=$(((size + 65535) / 65536)) 2>/dev/null | head -c "$size" > "$WORK/cur_kernel"
    if cmp -s "$WORK/cur_kernel" "$KDIR/zImage"; then
        log "Kernel $KERNEL unchanged."
    else
        log "Writing the new kernel $KERNEL... Do not power off!"
        erase_size=$(cat /sys/class/mtd/mtd3/erasesize)
        flash_erase "$BOOT_MTD" 0 $((DTB_OFFSET / erase_size)) >/dev/null || fail "flash_erase boot: reflash the firmware with RV1106_Toolkit"
        nandwrite -p -q "$BOOT_MTD" "$KDIR/zImage" || fail "kernel write: reflash the firmware with RV1106_Toolkit"
    fi
else
    KDIR=/data/boot/6.1
    dd if="$BOOT_MTD" of="$WORK/cur_kernel" bs=65536 count=$((DTB_OFFSET / 65536)) 2>/dev/null
    dd if="$WORK/boot.img" of="$WORK/new_kernel" bs=65536 count=$((DTB_OFFSET / 65536)) 2>/dev/null
    if cmp -s "$WORK/cur_kernel" "$WORK/new_kernel"; then
        log "Kernel unchanged."
    else
        log "Writing the new kernel (boot.img)... Do not power off!"
        size=$(wc -c < "$WORK/boot.img")
        flash_erase "$BOOT_MTD" 0 0 >/dev/null || fail "flash_erase boot: reflash the firmware with RV1106_Toolkit"
        mtd_debug write "$BOOT_MTD" 0 "$size" "$WORK/boot.img" >/dev/null || fail "boot write: reflash the firmware with RV1106_Toolkit"
    fi
fi
# a release older than the kernel switch keeps the DTBs in /data/boot
[ -d "$KDIR" ] || KDIR=/data/boot

# ---- DTB of the current I2S mode ----
case "$MODE:$MCLK" in
    ext:512) DTB=$KDIR/512_ext.dtb ;;
    ext:*)   DTB=$KDIR/1024_ext.dtb ;;
    *)       DTB=$KDIR/1024_pll.dtb ;;
esac
if [ -f "$DTB" ]; then
    n=$(wc -c < "$DTB")
    dd if="$BOOT_MTD" bs=65536 skip=$((DTB_OFFSET / 65536)) count=4 2>/dev/null | head -c "$n" > "$WORK/cur_dtb"
    if ! cmp -s "$WORK/cur_dtb" "$DTB"; then
        log "Writing the DTB for I2S mode $MODE, MCLK $MCLK..."
        flash_erase "$BOOT_MTD" $DTB_OFFSET 2 >/dev/null && \
            nandwrite -p -q "$BOOT_MTD" -s $DTB_OFFSET "$DTB" || fail "DTB write"
    fi
fi
sync

log "Done: $CUR_BUILD -> $NEW_BUILD"
log "Rebooting in 5 seconds..."
cleanup
( sleep 5; reboot ) >/dev/null 2>&1 &
exit 0
