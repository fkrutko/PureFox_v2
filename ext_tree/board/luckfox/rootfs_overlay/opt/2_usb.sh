#!/bin/sh

. /opt/audio-transition-lock.sh
. /opt/audio-card-utils.sh

configure_usb_asound || exit 1
echo USB > /etc/output
run_without_audio_lock_fd /etc/init.d/S40statusmonitor restart
sync
run_without_audio_lock_fd sh -c '/etc/init.d/S95* restart' 2>/dev/null || true
