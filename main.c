#include <json-c/json.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/inotify.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "config/config.h"
#include "ppd/ppd.h"

// drain the inotify fd. Returns true if config.json was changed.
static bool inotify_config_changed(int fd) {
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  bool changed = false;

  for (;;) {
    ssize_t len = read(fd, buf, sizeof(buf));
    if (len <= 0)
      break;
    for (char *p = buf; p < buf + len;) {
      struct inotify_event *ev = (struct inotify_event *)p;
      if (ev->len && strcmp(ev->name, CONFIG_FILE) == 0)
        changed = true;
      p += sizeof(*ev) + ev->len;
    }
  }
  return changed;
}

static int bus_timeout_ms(sd_bus *bus) {
  uint64_t t;
  if (sd_bus_get_timeout(bus, &t) < 0 || t == UINT64_MAX)
    return -1;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
  return t <= now ? 0 : (int)((t - now + 999) / 1000);
}

int main(void) {
  if (config_init() < 0)
    return 1;

  sd_bus *bus = NULL;
  int r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "Failed to open system bus: %s\n", strerror(-r));
    return 1;
  }

  r = ppd_init(bus);
  if (r < 0)
    return 1;

  int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (ifd < 0 ||
      inotify_add_watch(ifd, CONFIG_PATH, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
    perror("inotify");
    return 1;
  }

  for (;;) {
    r = sd_bus_process(bus, NULL);
    if (r < 0) {
      fprintf(stderr, "sd_bus_process failed: %s\n", strerror(-r));
      break;
    }
    if (r > 0)
      continue; // more messages may be pending, don't sleep yet

    struct pollfd fds[2] = {
        {.fd = sd_bus_get_fd(bus), .events = sd_bus_get_events(bus)},
        {.fd = ifd, .events = POLLIN},
    };
    if (poll(fds, 2, bus_timeout_ms(bus)) < 0 && errno != EINTR)
      break;

    if ((fds[1].revents & POLLIN) && inotify_config_changed(ifd)) {
      if (config_reload() == 0)
        ppd_reapply();
    }
  }

  sd_bus_flush_close_unref(bus);
  return 1;
}
