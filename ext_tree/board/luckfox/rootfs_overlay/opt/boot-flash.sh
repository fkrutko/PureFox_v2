#!/bin/sh
#
# Boot partition helpers, sourced by /opt/kernel.sh and the /opt/2*.sh I2S
# mode scripts.
#
# Boot partition (mtd3, 4 MiB): kernel zImage at 0, DTB at 0x3C0000.
# /data/boot/<kernel>/ holds zImage and the DTBs of each kernel choice: 6.1,
# 5.10, 1.6 (5.10 with the PureFox 1.6 I2S driver);
# /etc/kernel names the selected kernel (no file: 6.1).

BOOT_MTD=/dev/mtd3
DTB_OFFSET=3932160          # 0x3C0000, as in post-build.sh and U-Boot bootcmd

boot_kernel() {
    kernel=$(cat /etc/kernel 2>/dev/null)
    case "$kernel" in
        5.10|1.6) echo "$kernel" ;;
        *)        echo 6.1 ;;
    esac
}

boot_dir() {
    echo "/data/boot/$(boot_kernel)"
}

# DTB file name for the I2S mode in /etc/i2s.conf
mode_dtb() {
    mode=$(sed -n 's/^MODE=//p' /etc/i2s.conf 2>/dev/null)
    mclk=$(sed -n 's/^MCLK=//p' /etc/i2s.conf 2>/dev/null)
    case "$mode:$mclk" in
        ext:512) echo 512_ext.dtb ;;
        ext:*)   echo 1024_ext.dtb ;;
        *)       echo 1024_pll.dtb ;;
    esac
}

# boot_has <file> <offset>: the boot partition holds <file> at <offset>
boot_has() {
    n=$(wc -c < "$1") || return 1
    dd if="$BOOT_MTD" bs=65536 skip=$(($2 / 65536)) count=$(((n + 65535) / 65536)) 2>/dev/null \
        | head -c "$n" > /tmp/boot-flash.cmp
    cmp -s /tmp/boot-flash.cmp "$1"
    rc=$?
    rm -f /tmp/boot-flash.cmp
    return $rc
}

write_dtb() {
    flash_erase "$BOOT_MTD" $DTB_OFFSET 2 >/dev/null || return 1
    sleep 1
    nandwrite -p -q "$BOOT_MTD" -s $DTB_OFFSET "$1"
}

write_kernel() {
    erase_size=$(cat /sys/class/mtd/mtd3/erasesize 2>/dev/null)
    [ "${erase_size:-0}" -gt 0 ] || return 1
    flash_erase "$BOOT_MTD" 0 $((DTB_OFFSET / erase_size)) >/dev/null || return 1
    nandwrite -p -q "$BOOT_MTD" "$1"
}
