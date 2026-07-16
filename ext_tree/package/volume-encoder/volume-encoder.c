#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

static volatile sig_atomic_t running = 1;

static void stop(int sig) { (void)sig; running = 0; }

static int get_volume(void)
{
	FILE *pipe = popen("/usr/bin/amixer sget PCM 2>/dev/null", "r");
	char line[256];
	int volume = 100;

	if (!pipe)
		return volume;
	while (fgets(line, sizeof(line), pipe)) {
		char *start = strchr(line, '[');
		if (start && sscanf(start, "[%d%%]", &volume) == 1)
			break;
	}
	pclose(pipe);
	return volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

static void set_volume(int volume)
{
	char command[96];

	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;
	snprintf(command, sizeof(command), "/usr/bin/amixer -q sset PCM %d%%", volume);
	if (system(command) == -1)
		return;
}

static void notify_status(void)
{
	if (system("/opt/dbus_notify VolumeChanged encoder 2>/dev/null &") == -1)
		return;
}

static void toggle_mute(void)
{
	if (system("/usr/bin/amixer -q sset PCM toggle") == -1)
		return;
	notify_status();
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
	int count = 0, volume = get_volume(), dirty = 0;
	long long save_at = 0;
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
		int timeout = dirty ? SAVE_DELAY_MS : -1;
		int ready = poll(fds, count, timeout);
		clock_gettime(CLOCK_MONOTONIC, &now);
		long long ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;

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
						notify_status();
						dirty = 1;
						save_at = ms + SAVE_DELAY_MS;
					} else if (event.type == EV_KEY && event.code == KEY_MUTE && event.value) {
						toggle_mute();
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
