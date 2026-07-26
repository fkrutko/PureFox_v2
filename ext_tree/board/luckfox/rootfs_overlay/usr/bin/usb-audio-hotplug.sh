#!/bin/sh

# mdev passes variables via environment:
# $ACTION = add/remove
# $MDEV = device name (e.g. card1)

USB_CARD_STATE=/tmp/usb-audio-card
CARD_DEVICE=$(readlink -f "/sys/class/sound/$MDEV/device" 2>/dev/null || true)
IS_USB_CARD=0
case "$CARD_DEVICE" in
    */usb*) IS_USB_CARD=1 ;;
esac

# On remove, sysfs may already be gone. Remember the USB card seen on add so
# that a disappearing USB device still gets its services stopped cleanly.
case "$ACTION" in
    add)
        [ "$IS_USB_CARD" -eq 1 ] || exit 0
        echo "$MDEV" > "$USB_CARD_STATE"
        ;;
    remove)
        remembered_card=$(cat "$USB_CARD_STATE" 2>/dev/null || true)
        if [ "$IS_USB_CARD" -ne 1 ] && [ "$remembered_card" != "$MDEV" ]; then
            exit 0
        fi
        ;;
    *)
        exit 0
        ;;
esac

. /opt/audio-transition-lock.sh
. /opt/audio-card-utils.sh

# Debug logging
echo "mdev hotplug: ACTION=$ACTION DEVICE=$MDEV" >> /tmp/usb-hotplug.log

case "$ACTION" in
    add)
        # USB audio device connected
        echo "USB audio device connected: $MDEV" >> /tmp/usb-hotplug.log
        sleep 1
        if grep -qx USB /etc/output 2>/dev/null; then
            configure_usb_asound || exit 1
        fi
        # Stop then start audio services (cleaner than restart)
        for service in /etc/init.d/S95*; do
            if [ -x "$service" ]; then
                run_without_audio_lock_fd "$service" stop
                run_without_audio_lock_fd "$service" start &
            fi
        done
        run_without_audio_lock_fd /etc/init.d/S40statusmonitor restart
        ;;
    remove)
        # USB audio device disconnected
        echo "USB audio device disconnected: $MDEV" >> /tmp/usb-hotplug.log

        # Stop audio services to prevent 100% CPU usage (especially librespot)
        # They will be restarted when USB device is reconnected
        for service in /etc/init.d/S95*; do
            [ -x "$service" ] && run_without_audio_lock_fd "$service" stop &
        done
        if grep -qx USB /etc/output 2>/dev/null; then
            configure_usb_asound || exit 1
        fi
        rm -f "$USB_CARD_STATE"
        run_without_audio_lock_fd /etc/init.d/S40statusmonitor restart
        ;;
esac