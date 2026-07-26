#!/bin/sh

find_usb_sound_card() {
    for device_link in /sys/class/sound/card*/device; do
        [ -e "$device_link" ] || continue

        device_path=$(readlink -f "$device_link" 2>/dev/null || true)
        case "$device_path" in
            */usb*) ;;
            *) continue ;;
        esac

        card_name=${device_link%/device}
        card_name=${card_name##*/}
        card_number=${card_name#card}
        case "$card_number" in
            ''|*[!0-9]*) continue ;;
        esac

        printf '%s\n' "$card_number"
        return 0
    done

    return 1
}

usb_audio_device() {
    card_number=$(find_usb_sound_card) || return 1
    printf 'hw:%s\n' "$card_number"
}

configure_usb_asound() {
    if ! card_number=$(find_usb_sound_card); then
        rm -f /etc/asound.conf
        ln -s /etc/asound.usb /etc/asound.conf
        return 0
    fi

    temporary_config="/etc/asound.conf.$$"
    sed "s/card 1/card $card_number/g" /etc/asound.usb > "$temporary_config" || {
        rm -f "$temporary_config"
        return 1
    }
    mv "$temporary_config" /etc/asound.conf
}
