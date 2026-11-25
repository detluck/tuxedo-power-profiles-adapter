#include "tcc_client.h"
#include <json-c/json.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define TCCD_BUS "com.tuxedocomputers.tccd"
#define TCCD_PATH "/com/tuxedocomputers/tccd"
#define TCCD_INTERFACE "com.tuxedocomputers.tccd"
#define TCCD_METHOD_GET_ACTIVE_PROFILE "GetActiveProfileJSON"
#define TCCD_METHOD_GET_PROFILES "GetProfilesJSON"
#define TCCD_METHOD_SET_PROFILE "SetTempProfileById"
#define _cleanup_(f) __attribute__((cleanup(f)))

static sd_bus *tcc_bus = NULL;

static char *parse_profile(const char *ans) {
  struct json_object *root = json_tokener_parse(ans);
  if (!root) {
    fprintf(stderr, "Failed to parse active profile JSON\n");
    return NULL;
  }

  struct json_object *id_obj = NULL;
  if (!json_object_object_get_ex(root, "id", &id_obj) ||
      !json_object_is_type(id_obj, json_type_string)) {
    fprintf(stderr, "Missing 'id' field in active profile JSON\n");
    json_object_put(root);
    return NULL;
  }

  char *out = NULL;
  out = strdup(json_object_get_string(id_obj));
  json_object_put(root);
  return out;
}

static int parse_profiles_array(const char *ans, char ***out_ids,
                                size_t *out_count) {
  struct json_object *root = json_tokener_parse(ans);
  if (!root) {
    fprintf(stderr, "Failed to parse profiles JSON\n");
    return -EINVAL;
  }

  if (!json_object_is_type(root, json_type_array)) {
    fprintf(stderr, "Expected JSON array from GetProfilesJSON\n");
    json_object_put(root);
    return -EINVAL;
  }

  size_t len = json_object_array_length(root);
  char **ids = calloc(len, sizeof(char *));
  if (!ids && len > 0) {
    json_object_put(root);
    return -ENOMEM;
  }

  for (size_t i = 0; i < len; i++) {
    struct json_object *item = json_object_array_get_idx(root, i);
    struct json_object *id_obj = NULL;

    if (!json_object_is_type(item, json_type_object) ||
        !json_object_object_get_ex(item, "id", &id_obj) ||
        !json_object_is_type(id_obj, json_type_string)) {
      fprintf(stderr, "Invalid profile item at index %zu\n", i);
      // Clean up any strings already allocated before failing
      for (size_t j = 0; j < i; j++) {
        free(ids[j]);
      }
      free(ids);
      json_object_put(root);
      return -EINVAL;
    }

    ids[i] = strdup(json_object_get_string(id_obj));
    if (!ids[i]) {
      for (size_t j = 0; j < i; j++) {
        free(ids[j]);
      }
      free(ids);
      json_object_put(root);
      return -ENOMEM;
    }
  }

  json_object_put(root);
  *out_ids = ids;
  *out_count = len;
  return 0;
}

int log_error(int error, const char *message) {
  fprintf(stderr, "%s: %s\n", message, strerror(-error));
  return error;
}

int tcc_client_init(void) { return sd_bus_open_system(&tcc_bus); }

void tcc_client_cleanup(void) {
  sd_bus_unref(tcc_bus);
  tcc_bus = NULL;
}

int tcc_get_profiles(char ***profile_ids, size_t *count) {
  if (!profile_ids || !count) {
    return -EINVAL;
  }
  *profile_ids = NULL;
  *count = 0;

  int r = tcc_client_init();
  if (r < 0) {
    return r;
  }

  _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
  _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;

  r = sd_bus_call_method(tcc_bus, TCCD_BUS, TCCD_PATH, TCCD_INTERFACE,
                         TCCD_METHOD_GET_PROFILES, &error, &reply, "");
  if (r < 0) {
    return log_error(r, TCCD_METHOD_GET_PROFILES " call failed");
  }

  const char *ans = NULL;
  r = sd_bus_message_read(reply, "s", &ans);
  if (r < 0) {
    return log_error(r, "Failed to read reply");
  }

  return parse_profiles_array(ans, profile_ids, count);
}

int tcc_get_active_profile(char **profile_id) {
  if (profile_id == NULL) {
    return -EINVAL;
  }
  _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
  _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;

  int r;
  if (!tcc_bus) {
    r = tcc_client_init();
    if (r < 0) {
      log_error(r, "Failed to connect to the system bus");
    }
  }

  r = sd_bus_call_method(tcc_bus, TCCD_BUS, TCCD_PATH, TCCD_INTERFACE,
                         TCCD_METHOD_GET_ACTIVE_PROFILE, &error, &reply, "");
  if (r < 0)
    return log_error(r, TCCD_METHOD_GET_PROFILES "call failed");

  const char *ans;
  r = sd_bus_message_read(reply, "s", &ans);
  if (r < 0)
    return log_error(r, "Failed to read reply");

  *profile_id = parse_profile(ans);

  if (!*profile_id) {
    return -ENOMEM;
  }
  return 0;
}

int tcc_set_profile(const char *profile_id) {
  if (profile_id == NULL) {
    return -EINVAL;
  }

  _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;

  int r;
  if (!tcc_bus) {
    r = tcc_client_init();
    if (r < 0) {
      log_error(r, "Failed to connect to the system bus");
    }
  }
  r = sd_bus_call_method(tcc_bus, TCCD_BUS, TCCD_PATH, TCCD_INTERFACE,
                         TCCD_METHOD_SET_PROFILE, &error, NULL, "s",
                         profile_id);

  if (r < 0)
    return log_error(r, TCCD_METHOD_GET_PROFILES "call failed");

  return 0;
}
