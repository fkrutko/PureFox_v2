#!/bin/sh
# Low noise settings: the "Low noise" tab of the web page. They do not change
# the audio data; they make the board quieter electrically while it plays.
# Kept in /etc/lownoise.conf.
#
#   lownoise.sh apply           apply every setting (boot, S12lownoise)
#   lownoise.sh set KEY VALUE   store one setting and apply it
#
#   CPU_MAX_FREQ   0 (kernel default) or a frequency in kHz from
#                  scaling_available_frequencies
#   I2S_DRIVE      default (as at boot: level 2) or 0..7: drive strength of
#                  the SCLK, LRCK and data output pins
#   MCLK_DRIVE     default (as at boot: level 3) or 0..7: drive strength of
#                  the MCLK pin (an input in EXT mode, then it has no effect)
#   DMA_PERIODS    16 (driver default), 8, 4 or 2: fewest DMA periods per
#                  ALSA buffer; fewer periods are longer, fewer interrupts
#   SCREAM_RESAMPLE  1 (default): ALSA resamples a rate the output does not
#                  take; 0: Scream fails instead

CONF=/etc/lownoise.conf
CPUFREQ=/sys/devices/system/cpu/cpu0/cpufreq
DMA_PARAM=/sys/module/snd_soc_rockchip_i2s_tdm/parameters/periods_min
SAVED=/tmp/lownoise            # values at boot, to go back to "default"

# I2S0 output pins on GPIO2: A0 SCLK, A1 LRCK, A2 MCLK, A3 SDO3, A4 SDO0,
# A6 SDO2, A7 SDO1 (MCLK_PIN takes MCLK_DRIVE). Drive strength: 2 pins per register, 8 bits per pin,
# write mask in the upper half; level N is (1 << (N + 1)) - 1.
I2S_PINS="0 1 2 3 4 6 7"
MCLK_PIN=2
DRV_BASE=$((0xff5480c0))

conf_get() {    # conf_get KEY DEFAULT
    v=$(sed -n "s/^$1=//p" "$CONF" 2>/dev/null | tail -n 1)
    echo "${v:-$2}"
}

conf_set() {    # conf_set KEY VALUE
    if grep -q "^$1=" "$CONF" 2>/dev/null; then
        sed -i "s/^$1=.*/$1=$2/" "$CONF"
    else
        echo "$1=$2" >> "$CONF"
    fi
}

drv_reg()   { echo $((DRV_BASE + $1 / 2 * 4)); }
drv_shift() { echo $(($1 % 2 * 8)); }

save_boot_values() {
    mkdir -p "$SAVED"
    if [ ! -f "$SAVED/cpu" ] && [ -d "$CPUFREQ" ]; then
        cat "$CPUFREQ/scaling_cur_freq" > "$SAVED/cpu"
    fi
    if [ ! -f "$SAVED/drive" ]; then
        for pin in $I2S_PINS; do
            val=$(io -4 -r "$(drv_reg $pin)" | awk '{print $2}')
            echo "$pin $(( (0x$val >> $(drv_shift $pin)) & 0xff ))"
        done > "$SAVED/drive.tmp" && mv "$SAVED/drive.tmp" "$SAVED/drive"
    fi
}

apply_cpu() {
    [ -d "$CPUFREQ" ] || return 0
    freq=$(conf_get CPU_MAX_FREQ 0)
    governor=$(cat "$CPUFREQ/scaling_governor")
    if [ "$freq" = 0 ]; then
        cat "$CPUFREQ/cpuinfo_max_freq" > "$CPUFREQ/scaling_max_freq"
        # the userspace governor (kernel 6.1) stays at the boot frequency
        [ "$governor" = userspace ] && cat "$SAVED/cpu" > "$CPUFREQ/scaling_setspeed"
    else
        echo "$freq" > "$CPUFREQ/scaling_max_freq"
        [ "$governor" = userspace ] && echo "$freq" > "$CPUFREQ/scaling_setspeed"
    fi
    return 0
}

apply_drive() {
    data_level=$(conf_get I2S_DRIVE default)
    mclk_level=$(conf_get MCLK_DRIVE default)
    while read -r pin boot_code; do
        level=$data_level
        [ "$pin" = "$MCLK_PIN" ] && level=$mclk_level
        if [ "$level" = default ]; then
            code=$boot_code
        else
            code=$(( (1 << (level + 1)) - 1 ))
        fi
        shift=$(drv_shift $pin)
        io -4 -w "$(drv_reg $pin)" "$(printf '0x%08x' $(( (0xff << (shift + 16)) | (code << shift) )))"
    done < "$SAVED/drive"
}

apply_dma() {
    [ -w "$DMA_PARAM" ] && conf_get DMA_PERIODS 16 > "$DMA_PARAM"
    return 0
}

# Scream opens the device once; restart it so it picks up the change.
restart_scream() {
    pidof scream > /dev/null && /etc/rc.pure/S95scream restart > /dev/null 2>&1
    return 0
}

valid() {   # valid KEY VALUE
    case "$1" in
        CPU_MAX_FREQ)
            [ "$2" = 0 ] && return 0
            [ -f "$CPUFREQ/scaling_available_frequencies" ] &&
                tr ' ' '\n' < "$CPUFREQ/scaling_available_frequencies" | grep -qx "$2" ;;
        I2S_DRIVE|MCLK_DRIVE) case "$2" in default|[0-7]) return 0 ;; esac; return 1 ;;
        DMA_PERIODS)   case "$2" in 2|4|8|16) return 0 ;; esac; return 1 ;;
        SCREAM_RESAMPLE) case "$2" in 0|1) return 0 ;; esac; return 1 ;;
        *) return 1 ;;
    esac
}

save_boot_values

case "$1" in
    apply)
        apply_cpu
        apply_drive
        apply_dma
        # SCREAM_RESAMPLE is read when Scream starts
        ;;
    set)
        valid "$2" "$3" || { echo "Invalid setting: $2=$3"; exit 1; }
        conf_set "$2" "$3"
        case "$2" in
            CPU_MAX_FREQ)  apply_cpu ;;
            I2S_DRIVE|MCLK_DRIVE) apply_drive ;;
            DMA_PERIODS)   apply_dma; restart_scream ;;
            SCREAM_RESAMPLE) restart_scream ;;
        esac
        ;;
    *)
        echo "Usage: $0 apply | set KEY VALUE"
        exit 1
        ;;
esac
exit 0
