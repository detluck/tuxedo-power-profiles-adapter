#include <json-c/json.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "tcc/tcc_client.h"

#define CONFIG_PATH "/etc/tcc-power-profiles/config.json"

static char power_saver_profile[64];
static char balanced_profile[64];
static char performance_profile[64];

static int load_config(void) {
  struct json_object *config = json_object_from_file(CONFIG_PATH);

  if (!config) {
    fprintf(stderr, "Failed to parse JSON file: %s\n",
            json_util_get_last_err());
    return -1;
  }

  struct json_object *profiles;

  if (!json_object_object_get_ex(config, "profiles", &profiles) ||
      !json_object_is_type(profiles, json_type_object)) {
    fprintf(stderr, "Missing profiles configuration\n");
    json_object_put(config);
    return -1;
  }

  struct {
    const char *key;
    char *destination;
    size_t capacity;
  } mappings[] = {
      {"power-saver", power_saver_profile, sizeof(power_saver_profile)},
      {"balanced", balanced_profile, sizeof(balanced_profile)},
      {"performance", performance_profile, sizeof(performance_profile)}};

  for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); i++) {
    struct json_object *value;

    if (!json_object_object_get_ex(profiles, mappings[i].key, &value) ||
        !json_object_is_type(value, json_type_string)) {
      fprintf(stderr, "Missing or invalid profile: %s\n", mappings[i].key);
      json_object_put(config);
      return -1;
    }

    const char *profile_id = json_object_get_string(value);

    if (!profile_id || strlen(profile_id) >= mappings[i].capacity) {
      fprintf(stderr, "Invalid or too long profile ID: %s\n", mappings[i].key);
      json_object_put(config);
      return -1;
    }

    strcpy(mappings[i].destination, profile_id);
  }

  json_object_put(config);
  return 0;
}

int main(void) {
  if (load_config() < 0) {
    return 1;
  }

  int fd = inotify_init1(IN_NONBLOCK);
  int wd = inotify_add_watch(fd, CONFIG_PATH,
                             IN_CLOSE_WRITE | IN_MOVE_SELF | IN_CREATE);
  char buffer[4096];

  ssize_t length = read(fd, buffer, sizeof(buffer));
  if (length > 0) {
    load_config();
  }

  char **profiles = NULL;
  size_t count = 0;

  int r = tcc_get_profiles(&profiles, &count);
  if (r < 0) {
    fprintf(stderr, "Failed to get profiles: %s\n", strerror(-r));
    return 1;
  }

  for (size_t i = 0; i < count; i++) {
    printf("Profile[%zu]: %s\n", i, profiles[i]);
    free(profiles[i]); // Free each individual string
  }
  free(profiles);

  char *active = NULL;

  r = tcc_get_active_profile(&active);
  if (r < 0) {
    fprintf(stderr, "Failed to get active TUXEDO profile: %s\n", strerror(-r));
    return 1;
  }
  printf("Active: %s\n", active);

  r = tcc_set_profile(power_saver_profile);
  if (r < 0) {
    fprintf(stderr, "Failed to get active TUXEDO profile: %s\n", strerror(-r));
    return 1;
  }
  r = tcc_get_active_profile(&active);
  if (r < 0) {
    fprintf(stderr, "Failed to get active TUXEDO profile: %s\n", strerror(-r));
    return 1;
  }
  printf("Active: %s\n ", active);

  tcc_client_cleanup();
  free(active);
  return 0;
}
