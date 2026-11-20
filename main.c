#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

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
  struct json_object *power_saver_obj = NULL;
  struct json_object *balanced_obj = NULL;
  struct json_object *performance_obj = NULL;

  json_object_object_get_ex(profiles, "power-saver", &power_saver_obj);
  json_object_object_get_ex(profiles, "balanced", &balanced_obj);
  json_object_object_get_ex(profiles, "performance", &performance_obj);

  const char *power_saver =
      power_saver_obj ? json_object_get_string(power_saver_obj) : NULL;
  const char *balanced =
      balanced_obj ? json_object_get_string(balanced_obj) : NULL;
  const char *performance =
      performance_obj ? json_object_get_string(performance_obj) : NULL;

  strncpy(power_saver_profile, power_saver, sizeof(power_saver_profile) - 1);

  strncpy(balanced_profile, balanced, sizeof(balanced_profile) - 1);

  strncpy(performance_profile, performance, sizeof(performance_profile) - 1);

  json_object_put(config);
  return 0;
}

int main(void) {
  if (load_config() < 0) {
    return 1;
  }
  printf("Profiles: %s, %s, %s", power_saver_profile, balanced_profile,
         performance_profile);
  return 0;
}
