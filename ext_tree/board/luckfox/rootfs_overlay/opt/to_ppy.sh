#!/bin/sh
#
# Back to the upstream firmware of ppy2 (github.com/ppy2/PureFox_v2, MAX_6.X):
# the same steps as its /opt/update.sh, from its update server.
#   1. rsync the root filesystem from luckfox.puredsd.ru::luckfox2 over /
#      (this fork's files are deleted, upstream's keep-list is kept)
#   2. flash env, idblock, U-Boot and boot from the /data/*.img it brings
#   3. reboot
# After that the device updates from ppy2 only; to come back to this fork
# flash its image with RV1106_Toolkit.

SERVER="luckfox@luckfox.puredsd.ru::luckfox2"
export RSYNC_PASSWORD=luckfox    # upstream's public password
LOCK=/tmp/purefox-update.lock

log()  { echo "$*"; }
fail() { echo "ERROR: $*"; rmdir "$LOCK" 2>/dev/null; exit 1; }

# rsync deletes this script: run a copy from RAM
case "$0" in
    /tmp/*) ;;
    *) cp "$0" /tmp/to_ppy.sh && exec sh /tmp/to_ppy.sh "$@" ;;
esac

mkdir "$LOCK" 2>/dev/null || { echo "ERROR: an update is already running"; exit 1; }

log "Checking the ppy2 update server..."
rsync -q --timeout=30 "$SERVER/var/www/config.php" /tmp/ppy_config.php \
    || fail "luckfox.puredsd.ru not reachable"
rm -f /tmp/ppy_config.php

log "Stopping players..."
for s in /etc/init.d/S95*; do [ -x "$s" ] && "$s" stop >/dev/null 2>&1; done
killall status_monitor 2>/dev/null
rm -f /data/*.img

log "Downloading the ppy2 firmware (a few minutes)..."
rsync -ac --timeout=120 --delete-before --one-file-system \
    --exclude=.git \
    --exclude=/dev --exclude=/proc --exclude=/sys --exclude=/mnt --exclude=/root \
    --exclude=/tmp --exclude=/run \
    --filter='protect /usr/aprenderer/*.dat' \
    --filter='protect /usr/aplayer/*.dat' \
    --filter='protect /data/ethaddr.txt' \
    --filter='protect /etc/resolv.conf' \
    --filter='protect /etc/init.d/S95*' \
    --filter='protect /var/www/radio.json' \
    "$SERVER" / || fail "rsync failed: the system may be partly replaced, run it again (ssh: /tmp/to_ppy.sh)"

# the player link is kept: this fork's Scream is APScream upstream
for link in /etc/init.d/S95*; do
    [ -L "$link" ] || continue
    target=$(readlink "$link")
    [ -e "$target" ] && continue
    case "$target" in
        */S95scream) [ -e /etc/rc.pure/S95apscream ] && ln -sf /etc/rc.pure/S95apscream "$link" && continue ;;
    esac
    rm -f "$link"
done
sync

for img in env idblock uboot boot; do
    [ -f /data/$img.img ] || fail "the ppy2 firmware has no $img.img"
done
log "Writing the boot partitions... Do not power off!"
flash_erase /dev/mtd0 0 0 >/dev/null && mtd_debug write /dev/mtd0 0 262144 /data/env.img >/dev/null \
    || fail "env write: reflash the firmware with RV1106_Toolkit"
flash_erase /dev/mtd1 0 0 >/dev/null && mtd_debug write /dev/mtd1 0 262144 /data/idblock.img >/dev/null \
    || fail "idblock write: reflash the firmware with RV1106_Toolkit"
flash_erase /dev/mtd2 0 0 >/dev/null && mtd_debug write /dev/mtd2 0 524288 /data/uboot.img >/dev/null \
    || fail "U-Boot write: reflash the firmware with RV1106_Toolkit"
flash_erase /dev/mtd3 0 0 >/dev/null && mtd_debug write /dev/mtd3 0 4194304 /data/boot.img >/dev/null \
    || fail "boot write: reflash the firmware with RV1106_Toolkit"
rm -f /data/*.img
sync

log "Done: ppy2 firmware installed."
log "Rebooting in 5 seconds..."
rmdir "$LOCK" 2>/dev/null
( sleep 5; reboot ) >/dev/null 2>&1 &
exit 0
