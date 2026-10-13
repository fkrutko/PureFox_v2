#!/bin/sh

. /opt/audio-transition-lock.sh

sed -i 's/007c001c/007c003c/' /etc/init.d/S94ioi2s
sed -i 's/MODE=ext/MODE=pll/' /etc/i2s.conf
sed -i 's/MCLK=512/MCLK=1024/' /etc/i2s.conf

. /opt/boot-flash.sh
write_dtb "$(boot_dir)/1024_pll.dtb"

sync
#reboot -f
