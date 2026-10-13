#!/bin/sh

. /opt/audio-transition-lock.sh

sed -i 's/007c003c/007c001c/' /etc/init.d/S94ioi2s
sed -i 's/MCLK=512/MCLK=1024/' /etc/i2s.conf

. /opt/boot-flash.sh
write_dtb "$(boot_dir)/1024_ext.dtb"

sync
#reboot -f



