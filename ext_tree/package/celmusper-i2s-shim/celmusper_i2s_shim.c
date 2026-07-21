#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Buildroot enables _FILE_OFFSET_BITS=64, which aliases fopen to fopen64. */
#ifdef fopen
#undef fopen
#endif

/*
 * CelMusper discovers DACs from the USB-audio stream0 descriptor and then
 * opens the selected card as hw:<card>,0.  Rockchip I2S is not USB and has no
 * stream0 file, so expose a conservative virtual stereo PCM DAC for card 0.
 * The actual PCM open is redirected to "default", preserving asound.conf's
 * stereo-to-four-channel balanced-DAC routing.
 *
 * No samples are inspected or modified by this library.
 */
static const char i2s_stream0[] __attribute__((unused)) =
    "PureFox I2S at virtual-i2s : USB Audio\\n"
    "Playback:\\n"
    "  Status: Stop\\n"
    "  Interface 1\\n"
    "    Altset 1\\n"
    "    Format: S16_LE\\n"
    "    Channels: 2\\n"
    "    Endpoint: 0x01 (1 OUT) (ASYNC)\\n"
    "    Rates: 44100, 48000, 88200, 96000, 176400, 192000\\n"
    "    Bits: 16\\n"
    "    Channel map: FL FR\\n"
    "  Interface 1\\n"
    "    Altset 2\\n"
    "    Format: S24_3LE\\n"
    "    Channels: 2\\n"
    "    Endpoint: 0x01 (1 OUT) (ASYNC)\\n"
    "    Rates: 44100, 48000, 88200, 96000, 176400, 192000\\n"
    "    Bits: 24\\n"
    "    Channel map: FL FR\\n"
    "  Interface 1\\n"
    "    Altset 3\\n"
    "    Format: S32_LE\\n"
    "    Channels: 2\\n"
    "    Endpoint: 0x01 (1 OUT) (ASYNC)\\n"
    "    Rates: 44100, 48000, 88200, 96000, 176400, 192000\\n"
    "    Bits: 32\\n"
    "    Channel map: FL FR\\n";

static const char i2s_usbid[] __attribute__((unused)) = "f0f0:0001\\n";

static int card_file(const char *path, const char *leaf, int *card)
{
    char expected[64];
    int parsed;

    if (!path || sscanf(path, "/proc/asound/card%d/%63s", &parsed, expected) != 2)
        return 0;
    if (strcmp(expected, leaf) != 0)
        return 0;
    *card = parsed;
    return 1;
}

int celmusper_open(const char *path, int flags, ...) __asm__("open");
int celmusper_open(const char *path, int flags, ...)
{
    static int (*real_open)(const char *, int, ...);
    int card;
    mode_t mode = 0;
    va_list ap;

    if (!real_open)
        real_open = dlsym(RTLD_NEXT, "open");

    if ((flags & O_ACCMODE) == O_RDONLY && card_file(path, "usbid", &card)) {
        if (card == 0)
            return real_open("/tmp/celmusper-i2s-usbid", flags);
        errno = ENOENT;
        return -1;
    }
    if ((flags & O_ACCMODE) == O_RDONLY && card_file(path, "stream0", &card) &&
        card == 0) {
        return real_open("/tmp/celmusper-i2s-stream0", flags);
    }

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        return real_open(path, flags, mode);
    }
    return real_open(path, flags);
}

/* The transport imports the 32-bit uClibc symbol fopen.  Give the wrapper
 * that exact ABI name even when Buildroot enables large-file source aliases. */
FILE *celmusper_fopen(const char *path, const char *mode) __asm__("fopen");
FILE *celmusper_fopen(const char *path, const char *mode)
{
    static FILE *(*real_fopen)(const char *, const char *);
    int card;

    if (!real_fopen)
        real_fopen = dlsym(RTLD_NEXT, "fopen");

    if (mode && mode[0] == 'r' && card_file(path, "usbid", &card)) {
        if (card == 0)
            return real_fopen("/tmp/celmusper-i2s-usbid", "r");
        errno = ENOENT;
        return NULL;
    }

    if (mode && mode[0] == 'r' && card_file(path, "stream0", &card) && card == 0) {
        return real_fopen("/tmp/celmusper-i2s-stream0", "r");
    }

    return real_fopen(path, mode);
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name,
                 snd_pcm_stream_t stream, int mode)
{
    static int (*real_snd_pcm_open)(snd_pcm_t **, const char *,
                                    snd_pcm_stream_t, int);

    if (!real_snd_pcm_open)
        real_snd_pcm_open = dlsym(RTLD_NEXT, "snd_pcm_open");

    if (name && (!strcmp(name, "hw:0,0") || !strcmp(name, "hw:0")))
        name = "default";

    return real_snd_pcm_open(pcm, name, stream, mode);
}
