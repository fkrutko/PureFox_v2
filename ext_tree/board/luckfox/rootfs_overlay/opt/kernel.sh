#!/bin/sh
#
# Select the Linux kernel the device boots.
#
#   /opt/kernel.sh          print the running and the selected kernel
#   /opt/kernel.sh 6.1      boot kernel 6.1 (default)
#   /opt/kernel.sh 5.10     boot kernel 5.10 (the kernel of PureFox 1.x)
#   /opt/kernel.sh 1.6      boot kernel 5.10 with the I2S driver and DTBs of
#                           PureFox 1.6
#
# Writes the kernel and the DTB of the current I2S mode from /data/boot/<kernel>/
# to the boot partition, reads them back and keeps the choice in /etc/kernel.
# The new kernel starts after a reboot. 6.1 and 5.10 run the same I2S driver.

. /opt/boot-flash.sh

case "$1" in
    "")
        echo "Running:  $(uname -r)"
        echo "Selected: $(boot_kernel)"
        exit 0 ;;
    6.1|5.10|1.6) KERNEL=$1 ;;
    *)
        echo "Usage: $0 [6.1|5.10|1.6]" >&2
        exit 2 ;;
esac

if [ "$KERNEL" != 6.1 ] && [ -f /etc/usb_to_i2s.state ]; then
    echo "ERROR: USB to I2S works only with kernel 6.1. Turn it off first."
    exit 1
fi

DIR=/data/boot/$KERNEL
DTB=$DIR/$(mode_dtb)
for f in "$DIR/zImage" "$DTB"; do
    [ -f "$f" ] || { echo "ERROR: $f not found"; exit 1; }
done

# The I2S mode scripts write the DTB under the same lock
. /opt/audio-transition-lock.sh

# write_part <what> <file> <offset> <write function>: write unless already
# there, then read back; one retry
write_part() {
    boot_has "$2" "$3" && return 0
    echo "Writing $1... Do not power off!"
    for try in 1 2; do
        "$4" "$2" && boot_has "$2" "$3" && return 0
        echo "$1: read-back check failed (try $try)"
    done
    return 1
}

write_part "kernel $KERNEL" "$DIR/zImage" 0 write_kernel || {
    echo "ERROR: kernel write failed. Do not reboot: run '$0 $KERNEL' again,"
    echo "or reflash the firmware with RV1106_Toolkit if it keeps failing."
    exit 1
}
write_part "DTB $(mode_dtb)" "$DTB" $DTB_OFFSET write_dtb || {
    echo "ERROR: DTB write failed. Do not reboot: run '$0 $KERNEL' again."
    exit 1
}

echo "$KERNEL" > /etc/kernel
sync
echo "Kernel $KERNEL selected. It starts after a reboot."
