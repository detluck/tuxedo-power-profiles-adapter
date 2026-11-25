#include "config.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../tcc/tcc_client.h"

#define PPD_POWER_SAVER "power-saver"
#define PPD_BALANCED "balanced"
#define PPD_PERFORMANCE "performance"
#define PROFILE_LEN 64

enum { MODE_POWER_SAVER, MODE_BALANCED, MODE_PERFORMANCE, MODE_COUNT };

static const char *const ppd_names[MODE_COUNT] = {
    [MODE_POWER_SAVER] = "power-saver",
    [MODE_BALANCED] = "balanced",
    [MODE_PERFORMANCE] = "performance",
};

static char tcc_profiles[MODE_COUNT][PROFILE_LEN];

static int config_load(char out[MODE_COUNT][PROFILE_LEN]) {
  struct json_object *config = json_object_from_file(CONFIG_PATH);
  if (!config) {
    fprintf(stderr, "Failed to parse JSON file: %s\n",
            json_util_get_last_err());
    return -EINVAL;
  }

  struct json_object *profiles;
  if (!json_object_object_get_ex(config, "profiles", &profiles) ||
      !json_object_is_type(profiles, json_type_object)) {
    fprintf(stderr, "Missing profiles configuration\n");
    json_object_put(config);
    return -EINVAL;
  }

  for (int i = 0; i < MODE_COUNT; i++) {
    struct json_object *value;
    if (!json_object_object_get_ex(profiles, ppd_names[i], &value) ||
        !json_object_is_type(value, json_type_string)) {
      fprintf(stderr, "Missing or invalid profile: %s\n", ppd_names[i]);
      json_object_put(config);
      return -EINVAL;
    }

    const char *id = json_object_get_string(value);
    if (strlen(id) >= PROFILE_LEN) {
      fprintf(stderr, "Profile ID too long: %s\n", ppd_names[i]);
      json_object_put(config);
      return -ENAMETOOLONG;
    }
    strcpy(out[i], id);
  }

  json_object_put(config);
  return 0;
}

static bool tcc_has_profile(char **profiles, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++)
    if (strcmp(profiles[i], name) == 0)
      return true;
  return false;
}

static int config_validate(char cfg[MODE_COUNT][PROFILE_LEN]) {
  char **profiles = NULL;
  size_t count = 0;

  int r = tcc_get_profiles(&profiles, &count);
  if (r < 0) {
    fprintf(stderr, "Failed to get TCC profiles: %s\n", strerror(-r));
    return r;
  }

  int result = 0;
  for (int i = 0; i < MODE_COUNT; i++) {
    if (!tcc_has_profile(profiles, count, cfg[i])) {
      fprintf(stderr, "TCC has no profile '%s' (for %s)\n", cfg[i],
              ppd_names[i]);
      result = -ENOENT;
    }
  }

  for (size_t i = 0; i < count; i++)
    free(profiles[i]);
  free(profiles);
  return result;
}

int config_init(void) {
  char tmp[MODE_COUNT][PROFILE_LEN];
  int r = config_load(tmp);
  if (r < 0)
    return r;
  r = config_validate(tmp);
  if (r < 0)
    return r;
  memcpy(tcc_profiles, tmp, sizeof(tcc_profiles));
  return 0;
}

int config_reload(void) {
  int r = config_init();
  if (r < 0) {
    fprintf(stderr, "Failed to reload config %s\n", strerror(-r));
  }
  return r;
}

const char *config_tcc_profile(const char *ppd_name) {
  for (int i = 0; i < MODE_COUNT; i++)
    if (strcmp(ppd_names[i], ppd_name) == 0)
      return tcc_profiles[i];
  return NULL;
}

const char *config_ppd_profile(const char *tcc_name) {
  for (int i = 0; i < MODE_COUNT; i++)
    if (strcmp(tcc_profiles[i], tcc_name) == 0)
      return ppd_names[i];
  return NULL;
}
