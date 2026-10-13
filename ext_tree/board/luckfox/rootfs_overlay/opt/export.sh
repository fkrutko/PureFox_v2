#!/bin/sh

rm -f /etc/init.d/S95*

/opt/2pll.sh
sleep 1
mtd_debug read /dev/mtd0 0 262144 /data/env.img
sleep 1
mtd_debug read /dev/mtd1 0 262144 /data/idblock.img
sleep 1
mtd_debug read /dev/mtd2 0 524288 /data/uboot.img
sleep 1
mtd_debug read /dev/mtd3 0 4194304 /data/boot.img
sleep 1

rsync -axclHSzv --delete --one-file-system \
--exclude=/root/.bash_history  \
--exclude=/root/\.ssh/* \
--exclude=/var/tmp/systemd-private* \
--exclude=/var/log/* \
--exclude=/etc/resolv.conf \
--exclude=/data/ethaddr.txt \
--exclude=/root/* \
--exclude=/etc/init.d/S95* \
/  ppy@luckfox.puredsd.ru::purefox_v2

rm -f /data/*.img

