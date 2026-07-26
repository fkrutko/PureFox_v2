#!/bin/sh

. /opt/audio-transition-lock.sh

# Disable USBtoI2S mode: Switch USB to host mode, unlock ALSA toggle
MODE_FILE="/etc/usb_to_i2s.state"
UDC_SYSFS="/sys/kernel/config/usb_gadget"

# Clear mode file
rm -f "$MODE_FILE"

# 1. Stop UAC2 gadget SYNCHRONOUSLY (critical services)
run_without_audio_lock_fd /etc/init.d/S99uac2_router stop
run_without_audio_lock_fd /etc/init.d/S98uac2 stop
rm -f /etc/init.d/S98uac2
rm -f /etc/init.d/S99uac2_router

# 2. Unbind gadget from UDC
if [ -d "$UDC_SYSFS/purecore" ]; then
	echo "" > "$UDC_SYSFS/purecore/UDC" 2>/dev/null || true
fi

# 3. Switch USB to host mode
rmmod dwc3 2>/dev/null || true
sleep 0.2

# Load dwc3_host.ko
modprobe dwc3 2>/dev/null || true
sleep 0.5

# 4. Restart status monitor in BACKGROUND (can wait for services to stabilize)
run_without_audio_lock_fd /etc/init.d/S40statusmonitor restart >/dev/null 2>&1 &
sync

echo "USBtoI2S mode disabled, USB switched to host mode"
exit 0
