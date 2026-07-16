#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <alsa/asoundlib.h>
#include <dbus/dbus.h>
#include <linux/input.h>
#include <limits.h>
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
#define NOTIFY_DELAY_MS 75

#define DBUS_OBJECT_PATH "/org/purefox/statusmonitor"
#define DBUS_INTERFACE_NAME "org.purefox.StatusMonitor"

static volatile sig_atomic_t running = 1;

static void stop(int sig) { (void)sig; running = 0; }

static int open_pcm_mixer(snd_mixer_t **mixer, snd_mixer_elem_t **elem)
{
	snd_mixer_selem_id_t *id;

	*mixer = NULL;
	*elem = NULL;
	if (snd_mixer_open(mixer, 0) < 0 ||
	    snd_mixer_attach(*mixer, "default") < 0 ||
	    snd_mixer_selem_register(*mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(*mixer) < 0)
		goto error;

	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_name(id, "PCM");
	*elem = snd_mixer_find_selem(*mixer, id);
	if (*elem && snd_mixer_selem_has_playback_volume(*elem))
		return 0;

error:
	if (*mixer)
		snd_mixer_close(*mixer);
	*mixer = NULL;
	return -1;
}

static int get_volume(void)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	long min, max, value;
	int volume = 100;

	if (open_pcm_mixer(&mixer, &elem) < 0)
		return volume;
	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	if (max > min &&
	    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO,
					 &value) >= 0)
		volume = (int)((value - min) * 100 / (max - min));
	snd_mixer_close(mixer);
	return volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

static void set_volume(int volume)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	long min, max, value;

	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;
	if (open_pcm_mixer(&mixer, &elem) < 0)
		return;
	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	value = min + (max - min) * volume / 100;
	snd_mixer_selem_set_playback_volume_all(elem, value);
	snd_mixer_close(mixer);
}

static void notify_status(void)
{
	static DBusConnection *connection;
	DBusMessage *message;
	DBusError error;
	const char *source = "encoder";

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
	int enabled;

	if (open_pcm_mixer(&mixer, &elem) < 0)
		return;
	if (snd_mixer_selem_has_playback_switch(elem) &&
	    snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO,
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

int main(void)
{
	struct pollfd fds[EVENT_MAX];
	int count = 0, volume = get_volume(), dirty = 0, notify_pending = 0;
	long long save_at = 0, notify_at = 0;
	DIR *dir;
	struct dirent *entry;

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
		long long deadline = 0;
		int timeout;

		clock_gettime(CLOCK_MONOTONIC, &now);
		long long ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;
		if (dirty)
			deadline = save_at;
		if (notify_pending && (!deadline || notify_at < deadline))
			deadline = notify_at;
		timeout = deadline ? (int)(deadline > ms ? deadline - ms : 0) : -1;
		int ready = poll(fds, count, timeout);
		clock_gettime(CLOCK_MONOTONIC, &now);
		ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;

		if (ready > 0) {
			for (int i = 0; i < count; i++) {
				struct input_event event;
				while (read(fds[i].fd, &event, sizeof(event)) == sizeof(event)) {
					if (event.type == EV_REL && event.code == REL_DIAL && event.value) {
						volume = get_volume();
						volume += event.value;
						if (volume < 0)
							volume = 0;
						if (volume > 100)
							volume = 100;
						set_volume(volume);
						dirty = 1;
						save_at = ms + SAVE_DELAY_MS;
						notify_pending = 1;
						notify_at = ms + NOTIFY_DELAY_MS;
					} else if (event.type == EV_KEY && event.code == KEY_MUTE && event.value) {
						toggle_mute();
						notify_pending = 1;
						notify_at = ms;
					}
				}
			}
		}
		if (dirty && ms >= save_at) {
			save_volume(volume);
			dirty = 0;
		}
		if (notify_pending && ms >= notify_at) {
			notify_status();
			notify_pending = 0;
		}
	}
	if (dirty)
		save_volume(volume);
	return 0;
}
