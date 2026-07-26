#!/bin/sh

. /opt/audio-transition-lock.sh

rm -f /etc/asound.conf
ln -s /etc/asound.8ch /etc/asound.conf
sed -i 's/^SUBMODE=.*$/SUBMODE=8ch/' /etc/i2s.conf
echo I2S > /etc/output
run_without_audio_lock_fd /etc/init.d/S40statusmonitor restart
sync
run_without_audio_lock_fd sh -c '/etc/init.d/S95* restart' 2>/dev/null || true
