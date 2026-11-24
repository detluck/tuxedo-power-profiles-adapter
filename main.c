#include <json-c/json.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "config/config.h"

int main(void) {
  if (config_init() < 0) {
    return 1;
  }

  int fd = inotify_init1(IN_NONBLOCK);
  int wd = inotify_add_watch(fd, CONFIG_PATH,
                             IN_CLOSE_WRITE | IN_MOVE_SELF | IN_CREATE);
  char buffer[4096];

  ssize_t length = read(fd, buffer, sizeof(buffer));
  if (length > 0) {
    config_reload();
  }

  return 0;
}
