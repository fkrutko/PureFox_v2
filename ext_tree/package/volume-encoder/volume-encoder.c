#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <alsa/asoundlib.h>
#include <dbus/dbus.h>
#include <linux/input.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define EVENT_MAX 32
#define SAVE_DELAY_MS 1000
#define MIXER_CACHE_FILE "/tmp/mixer_control_cache"

#define DBUS_OBJECT_PATH "/org/purefox/statusmonitor"
#define DBUS_INTERFACE_NAME "org.purefox.StatusMonitor"

static volatile sig_atomic_t running = 1;

static void stop(int sig) { (void)sig; running = 0; }

static int find_usb_audio_card(void)
{
	DIR *sound_dir;
	struct dirent *entry;
	char device_link[PATH_MAX];
	char device_path[PATH_MAX];
	int card = -1;

	sound_dir = opendir("/sys/class/sound");
	if (!sound_dir)
		return -1;

	while ((entry = readdir(sound_dir)) != NULL) {
		if (strncmp(entry->d_name, "card", 4) != 0 ||
		    !isdigit((unsigned char)entry->d_name[4]))
			continue;

		snprintf(device_link, sizeof(device_link),
			 "/sys/class/sound/%s/device", entry->d_name);
		if (!realpath(device_link, device_path))
			continue;

		if (strstr(device_path, "/usb") != NULL) {
			card = (int)strtol(entry->d_name + 4, NULL, 10);
			break;
		}
	}

	closedir(sound_dir);
	return card;
}

static const char *active_mixer_device(void)
{
	FILE *file;
	char output[16];
	static char device_name[16];
	int usb_card;

	file = fopen("/etc/output", "r");
	if (file) {
		if (fgets(output, sizeof(output), file) &&
		    !strncmp(output, "USB", 3)) {
			fclose(file);
			usb_card = find_usb_audio_card();
			if (usb_card >= 0) {
				snprintf(device_name, sizeof(device_name), "hw:%d", usb_card);
				return device_name;
			}
			return NULL;
		}
		fclose(file);
	}
	return "hw:0";
}

static void set_cached_mixer_id(snd_mixer_selem_id_t *id)
{
	FILE *file;
	char line[96];
	char name[64];
	int index;

	snd_mixer_selem_id_set_name(id, "PCM");
	snd_mixer_selem_id_set_index(id, 0);
	file = fopen(MIXER_CACHE_FILE, "r");
	if (!file)
		return;
	if (fgets(line, sizeof(line), file) &&
	    sscanf(line, "'%63[^']',%d", name, &index) == 2) {
		snd_mixer_selem_id_set_name(id, name);
		snd_mixer_selem_id_set_index(id, index);
	}
	fclose(file);
}

static int open_pcm_mixer(snd_mixer_t **mixer, snd_mixer_elem_t **elem)
{
	snd_mixer_selem_id_t *id;
	const char *device;

	*mixer = NULL;
	*elem = NULL;
	device = active_mixer_device();
	if (!device || snd_mixer_open(mixer, 0) < 0 ||
	    snd_mixer_attach(*mixer, device) < 0 ||
	    snd_mixer_selem_register(*mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(*mixer) < 0)
		goto error;

	snd_mixer_selem_id_alloca(&id);
	set_cached_mixer_id(id);
	*elem = snd_mixer_find_selem(*mixer, id);
	if (!*elem) {
		snd_mixer_selem_id_set_name(id, "PCM");
		snd_mixer_selem_id_set_index(id, 0);
		*elem = snd_mixer_find_selem(*mixer, id);
	}
	if (*elem && snd_mixer_selem_has_playback_volume(*elem))
		return 0;

error:
	if (*mixer)
		snd_mixer_close(*mixer);
	*mixer = NULL;
	return -1;
}

static int playback_channel(snd_mixer_elem_t *elem,
			    snd_mixer_selem_channel_id_t *channel)
{
	static const snd_mixer_selem_channel_id_t channels[] = {
		SND_MIXER_SCHN_FRONT_LEFT,
		SND_MIXER_SCHN_FRONT_RIGHT,
		SND_MIXER_SCHN_MONO,
	};
	size_t i;
	long value;

	for (i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
		if (snd_mixer_selem_get_playback_volume(elem, channels[i],
						&value) >= 0) {
			*channel = channels[i];
			return 0;
		}
	}
	return -1;
}

static int get_volume(void)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	snd_mixer_selem_channel_id_t channel;
	long min, max, value;
	long db_min, db_max, db_value;
	double normalized, min_normalized;
	int volume = 100;

	if (open_pcm_mixer(&mixer, &elem) < 0)
		return volume;
	if (playback_channel(elem, &channel) < 0) {
		snd_mixer_close(mixer);
		return volume;
	}
	if (snd_mixer_selem_get_playback_dB_range(elem, &db_min, &db_max) >= 0 &&
	    db_min < db_max &&
	    snd_mixer_selem_get_playback_dB(elem, channel, &db_value) >= 0) {
		if (db_max - db_min <= 2400) {
			normalized = (double)(db_value - db_min) / (db_max - db_min);
		} else {
			normalized = pow(10.0, (double)(db_value - db_max) / 6000.0);
			min_normalized = pow(10.0, (double)(db_min - db_max) / 6000.0);
			normalized = (normalized - min_normalized) / (1.0 - min_normalized);
		}
		volume = (int)lround(normalized * 100.0);
	} else {
		snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
		if (max > min &&
		    snd_mixer_selem_get_playback_volume(elem, channel, &value) >= 0)
			volume = (int)(((value - min) * 100 + (max - min) / 2) /
				       (max - min));
	}
	snd_mixer_close(mixer);
	return volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

static int set_volume(int volume)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	long min, max, value;
	long db_min, db_max, db_value;
	double normalized, min_normalized;

	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;
	if (open_pcm_mixer(&mixer, &elem) < 0)
		return -1;
	if (snd_mixer_selem_get_playback_dB_range(elem, &db_min, &db_max) >= 0 &&
	    db_min < db_max) {
		normalized = (double)volume / 100.0;
		if (db_max - db_min <= 2400) {
			db_value = lround(normalized * (db_max - db_min)) + db_min;
		} else {
			min_normalized = pow(10.0, (double)(db_min - db_max) / 6000.0);
			normalized = normalized * (1.0 - min_normalized) + min_normalized;
			if (normalized <= 0.0)
				normalized = 1e-36;
			db_value = lround(6000.0 * log10(normalized)) + db_max;
		}
		if (snd_mixer_selem_set_playback_dB_all(elem, db_value, 0) < 0) {
			snd_mixer_close(mixer);
			return -1;
		}
	} else {
		snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
		value = min + (max - min) * volume / 100;
		if (snd_mixer_selem_set_playback_volume_all(elem, value) < 0) {
			snd_mixer_close(mixer);
			return -1;
		}
	}
	snd_mixer_close(mixer);
	return 0;
}

/*
 * Some USB mixers advertise a wide dB range but only expose a small number
 * of physical steps (the XMOS control is 128 steps, one dB each).  Mapping a
 * one-percent encoder movement through the psychoacoustic dB curve then
 * rounds 100, 99, 98 ... back to the same 0 dB hardware value.  For those
 * coarse controls, move by one actual mixer step so that every detent has an
 * audible effect.  Fine controls retain the common percentage mapping.
 */
static int step_volume(int delta)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	snd_mixer_selem_channel_id_t channel;
	long min, max, value, db_min, db_max;

	if (open_pcm_mixer(&mixer, &elem) < 0)
		return get_volume();
	if (playback_channel(elem, &channel) < 0) {
		snd_mixer_close(mixer);
		return get_volume();
	}
	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	if (max > min && max - min <= 128 &&
	    snd_mixer_selem_get_playback_dB_range(elem, &db_min, &db_max) >= 0 &&
	    db_max - db_min > 2400 &&
	    snd_mixer_selem_get_playback_volume(elem, channel, &value) >= 0) {
		value += delta;
		if (value < min)
			value = min;
		if (value > max)
			value = max;
		snd_mixer_selem_set_playback_volume_all(elem, value);
		snd_mixer_close(mixer);
		return get_volume();
	}
	snd_mixer_close(mixer);
	value = get_volume() + delta;
	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;
	set_volume(value);
	return get_volume();
}

static void notify_status(int volume)
{
	static DBusConnection *connection;
	DBusMessage *message;
	DBusError error;
	char value[8];
	const char *source;

	if (volume >= 0) {
		snprintf(value, sizeof(value), "%d%%", volume);
		source = value;
	} else {
		source = "mute";
	}

	if (!connection) {
		dbus_error_init(&error);
		connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
		if (dbus_error_is_set(&error)) {
			dbus_error_free(&error);
			return;
		}
		if (!connection)
			return;
	}
	message = dbus_message_new_signal(DBUS_OBJECT_PATH,
					 DBUS_INTERFACE_NAME, "VolumeChanged");
	if (!message)
		return;
	dbus_message_append_args(message, DBUS_TYPE_STRING, &source,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(connection, message, NULL);
	dbus_connection_flush(connection);
	dbus_message_unref(message);
}

static void toggle_mute(void)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	snd_mixer_selem_channel_id_t channel;
	int enabled;

	if (open_pcm_mixer(&mixer, &elem) < 0)
		return;
	if (playback_channel(elem, &channel) < 0) {
		snd_mixer_close(mixer);
		return;
	}
	if (snd_mixer_selem_has_playback_switch(elem) &&
	    snd_mixer_selem_get_playback_switch(elem, channel,
					    &enabled) >= 0)
		snd_mixer_selem_set_playback_switch_all(elem, !enabled);
	snd_mixer_close(mixer);
}

static void save_volume(int volume)
{
	FILE *file = fopen("/data/i2s_volume.tmp", "w");

	if (!file)
		return;
	fprintf(file, "%d\n", volume);
	fclose(file);
	rename("/data/i2s_volume.tmp", "/data/i2s_volume");
}

static int open_event(const char *name)
{
	char path[PATH_MAX], device_name[128] = "";
	int fd;

	snprintf(path, sizeof(path), "/dev/input/%s", name);
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (ioctl(fd, EVIOCGNAME(sizeof(device_name)), device_name) < 0 ||
	    (strcmp(device_name, "volume-encoder") &&
	     strcmp(device_name, "volume-encoder-keys") &&
	     strcmp(device_name, "PureFox Volume Encoder") &&
	     strcmp(device_name, "Volume Mute"))) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	struct pollfd fds[EVENT_MAX];
	int count = 0, volume = get_volume(), dirty = 0;
	long long save_at = 0;
	DIR *dir;
	struct dirent *entry;

	if (argc == 2 && !strcmp(argv[1], "mute")) {
		toggle_mute();
		return 0;
	}
	if (argc == 3 && (!strcmp(argv[1], "set") || !strcmp(argv[1], "adjust"))) {
		char *end;
		long value = strtol(argv[2], &end, 10);

		if (*argv[2] == '\0' || *end != '\0')
			return 1;
		if (!strcmp(argv[1], "adjust"))
			value += get_volume();
		return set_volume((int)value) ? 1 : 0;
	}
	if (argc != 1)
		return 1;

	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	dir = opendir("/dev/input");
	if (!dir)
		return 1;
	while ((entry = readdir(dir)) && count < EVENT_MAX) {
		if (strncmp(entry->d_name, "event", 5))
			continue;
		fds[count].fd = open_event(entry->d_name);
		if (fds[count].fd >= 0)
			fds[count++].events = POLLIN;
	}
	closedir(dir);
	if (!count)
		return 0;

	while (running) {
		struct timespec now;
		int timeout;

		clock_gettime(CLOCK_MONOTONIC, &now);
		long long ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;
		timeout = dirty ? (int)(save_at > ms ? save_at - ms : 0) : -1;
		int ready = poll(fds, count, timeout);
		clock_gettime(CLOCK_MONOTONIC, &now);
		ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;

		if (ready > 0) {
			for (int i = 0; i < count; i++) {
				struct input_event event;
				while (read(fds[i].fd, &event, sizeof(event)) == sizeof(event)) {
					if (event.type == EV_REL && event.code == REL_DIAL && event.value) {
						volume = step_volume(event.value);
						dirty = 1;
						save_at = ms + SAVE_DELAY_MS;
						notify_status(volume);
					} else if (event.type == EV_KEY && event.code == KEY_MUTE && event.value) {
						toggle_mute();
						notify_status(-1);
					}
				}
			}
		}
		if (dirty && ms >= save_at) {
			save_volume(volume);
			dirty = 0;
		}
	}
	if (dirty)
		save_volume(volume);
	return 0;
}
