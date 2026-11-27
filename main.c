#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "config/config.h"
#include "ppd/ppd.h"
#include "tcc/tcc_client.h"

#define TCCD_NAME "com.tuxedocomputers.tccd"

static volatile sig_atomic_t running = 1;

static int name_has_owner(sd_bus *bus, const char *name) {
  sd_bus_message *reply = NULL;
  int has = 0;

  int r = sd_bus_call_method(bus, "org.freedesktop.DBus",
                             "/org/freedesktop/DBus", "org.freedesktop.DBus",
                             "NameHasOwner", NULL, &reply, "s", name);
  if (r < 0)
    return r;

  r = sd_bus_message_read(reply, "b", &has);
  sd_bus_message_unref(reply);
  return r < 0 ? r : has;
}

static int wait_for_tccd(sd_bus *bus, int timeout_s) {
  struct timespec ts = {.tv_nsec = 500 * 1000 * 1000};

  for (int i = 0; i < timeout_s * 2; i++) {
    int r = name_has_owner(bus, TCCD_NAME);
    if (r < 0)
      return r;
    if (r > 0)
      return 0;
    nanosleep(&ts, NULL);
  }
  return -ETIMEDOUT;
}
static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

// drain the inotify fd. Returns true if config.json was changed.
static bool inotify_config_changed(int fd) {
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  bool changed = false;

  for (;;) {
    ssize_t len = read(fd, buf, sizeof(buf));
    if (len < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (len == 0)
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

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
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

#define SYNC_INTERVAL_MS 1000ULL

int main(void) {
  struct sigaction sa = {.sa_handler = handle_signal};
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  int exit_code = 1;
  sd_bus *bus = NULL;
  int ifd = -1;

  int r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "Failed to open system bus: %s\n", strerror(-r));
    goto cleanup;
  }

  r = wait_for_tccd(bus, 30);
  if (r < 0) {
    fprintf(stderr, "tccd did not appear on the bus: %s\n", strerror(-r));
    goto cleanup;
  }

  if (config_init() < 0)
    goto cleanup;

  r = ppd_init(bus);
  if (r < 0)
    goto cleanup;

  ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (ifd < 0 ||
      inotify_add_watch(ifd, CONFIG_DIR, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
    perror("inotify");
    goto cleanup;
  }

  uint64_t next_sync = monotonic_ms() + SYNC_INTERVAL_MS;

  while (running) {
    r = sd_bus_process(bus, NULL);
    if (r < 0) {
      fprintf(stderr, "sd_bus_process failed: %s\n", strerror(-r));
      goto cleanup;
    }
    if (r > 0)
      continue; // more messages may be pending, don't sleep yet

    uint64_t now = monotonic_ms();
    if (now >= next_sync) {
      ppd_sync(bus);
      now = monotonic_ms();
      next_sync = now + SYNC_INTERVAL_MS;
    }

    int timeout_ms = bus_timeout_ms(bus);
    int sync_ms = (int)(next_sync - now);
    if (timeout_ms < 0 || sync_ms < timeout_ms)
      timeout_ms = sync_ms;

    struct pollfd fds[2] = {
        {.fd = sd_bus_get_fd(bus), .events = sd_bus_get_events(bus)},
        {.fd = ifd, .events = POLLIN},
    };
    if (poll(fds, 2, timeout_ms) < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      goto cleanup;
    }

    if ((fds[1].revents & POLLIN) && inotify_config_changed(ifd)) {
      if (config_reload() == 0)
        ppd_reapply();
    }

    now = monotonic_ms();
    if (now >= next_sync) {
      ppd_sync(bus);
      next_sync = monotonic_ms() + SYNC_INTERVAL_MS;
    }
  }

  exit_code = 0;

cleanup:
  if (ifd >= 0)
    close(ifd);
  if (bus)
    sd_bus_flush_close_unref(bus);
  ppd_cleanup();
  tcc_client_cleanup();
  return exit_code;
}
