#include "ppd.h"

#include <asm-generic/errno-base.h>
#include <errno.h>
#include <iso646.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../config/config.h"
#include "../tcc/tcc_client.h"

#define PPD_VERSION "0.30"

#define PPD_INTERFACE "org.freedesktop.UPower.PowerProfiles"
#define PPD_PATH "/org/freedesktop/UPower/PowerProfiles"

// methods
#define PPD_METHOD_HOLD_PROFILE "HoldProfile"
#define PPD_METHOD_RELEASE_PROFILE "ReleaseProfile"

// signal
#define PPD_SIGNAL_PROFILE_RELEASED "ProfileReleased"

// props
#define PPD_PROP_ACTIVE_PROFILE "ActiveProfile"
#define PPD_PROP_PERFORMANCE_DEGRADED "PerformanceDegraded"
#define PPD_PROP_PROFILES "Profiles"
#define PPD_PROP_ACTIONS "Actions"
#define PPD_PROP_ACTIVE_PROFILE_HOLDS "ActiveProfileHolds"
#define PPD_PROP_VERSION "Version"

struct hold {
  uint32_t cookie;
  char *profile, *reason, *app_id, *sender;
  struct hold *next;
};

struct state {
  char *active;
  char *base;
  uint32_t next_cookie;
  struct hold *holds;
};

static int apply_profile(struct state *s, const char *profile) {
  const char *tcc_name = config_tcc_profile(profile);
  if (!tcc_name)
    return -EINVAL;

  int r = tcc_set_profile(tcc_name);
  if (r < 0)
    return r;

  free(s->active);
  s->active = strdup(profile);
  return s->active ? 0 : -ENOMEM;
}

static int init_state(struct state *s) {
  const char *initial = "balanced"; // fallback
  char *tcc_active = NULL;

  int r = tcc_get_active_profile(&tcc_active);
  if (r >= 0 && tcc_active) {
    const char *mapped = config_ppd_profile(tcc_active);
    if (mapped)
      initial = mapped;
  } else {
    log_error(r, "Failed to get active TCC profile, assuming balanced:");
  }
  free(tcc_active);

  s->active = strdup(initial);
  s->base = strdup(initial);
  return (s->active && s->base) ? 0 : -ENOMEM;
}

static const char *effective_hold_profile(struct state *s) {
  const char *result = NULL;
  for (struct hold *h = s->holds; h; h = h->next) {
    if (strcmp(h->profile, "power-saver") == 0)
      return "power-saver"; // always wins
    result = h->profile;
  }
  return result; // NULL if no holds
}

static void free_hold(struct hold *h) {
  free(h->profile);
  free(h->reason);
  free(h->app_id);
  free(h->sender);
  free(h);
}

static int method_hold_profile(sd_bus_message *m, void *userdata,
                               sd_bus_error *error) {
  struct state *s = userdata;
  const char *profile, *reason, *app_id;

  int r = sd_bus_message_read(m, "sss", &profile, &reason, &app_id);
  if (r < 0) {
    return log_error(r, "Failed to read dbus message:");
  }

  // ppd only allows holding these two
  if (strcmp(profile, "power-saver") != 0 &&
      strcmp(profile, "performance") != 0)
    return sd_bus_error_setf(
        error, SD_BUS_ERROR_INVALID_ARGS,
        "Only 'power-saver' and 'performance' can be held");

  struct hold *h = calloc(1, sizeof(*h));
  if (!h)
    return -ENOMEM;

  h->cookie = ++s->next_cookie;
  h->profile = strdup(profile);
  h->reason = strdup(reason);
  h->app_id = strdup(app_id);
  h->sender = strdup(sd_bus_message_get_sender(m));
  if (!h->profile || !h->reason || !h->app_id || !h->sender) {
    free_hold(h);
    return -ENOMEM;
  }

  h->next = s->holds;
  s->holds = h;
  const char *target = effective_hold_profile(s);
  r = apply_profile(s, target);
  if (r < 0) {
    s->holds = h->next;
    return log_error(r, "Failed to apply held profile:");
  }

  sd_bus *bus = sd_bus_message_get_bus(m);
  if (!bus) {
    return -EINVAL;
  }
  sd_bus_emit_properties_changed(bus, PPD_PATH, PPD_INTERFACE,
                                 PPD_PROP_ACTIVE_PROFILE,
                                 PPD_PROP_ACTIVE_PROFILE_HOLDS, NULL);

  return sd_bus_reply_method_return(m, "u", h->cookie);
}

static int method_release_profile(sd_bus_message *m, void *userdata,
                                  sd_bus_error *error) {
  struct state *s = userdata;
  uint32_t cookie;

  int r = sd_bus_message_read(m, "u", &cookie);
  if (r < 0) {
    return log_error(r, "Failed to read dbus message:");
  }

  for (struct hold **p = &s->holds; *p; p = &(*p)->next) {
    if ((*p)->cookie != cookie)
      continue;

    struct hold *h = *p;
    *p = h->next;
    free_hold(h);

    const char *target = effective_hold_profile(s);
    if (!target) {
      target = s->base;
    }
    r = apply_profile(s, target);
    if (r < 0) {
      log_error(r, "Failed to apply profile after release:");
    }

    sd_bus *bus = sd_bus_message_get_bus(m);
    sd_bus_emit_properties_changed(bus, PPD_PATH, PPD_INTERFACE,
                                   PPD_PROP_ACTIVE_PROFILE,
                                   PPD_PROP_ACTIVE_PROFILE_HOLDS, NULL);

    return sd_bus_reply_method_return(m, "");
  }
  return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "No such hold: %u",
                           cookie);
}

static int emit_profile_released(sd_bus *bus, struct hold *h) {
  sd_bus_message *sig = NULL;
  int r = sd_bus_message_new_signal(bus, &sig, PPD_PATH, PPD_INTERFACE,
                                    PPD_SIGNAL_PROFILE_RELEASED);
  if (r < 0)
    return r;

  r = sd_bus_message_set_destination(sig, h->sender);
  if (r >= 0)
    r = sd_bus_message_append(sig, "u", h->cookie);
  if (r >= 0)
    r = sd_bus_send(bus, sig, NULL);

  sd_bus_message_unref(sig);
  return r;
}

static int prop_active_profile_get(sd_bus *bus, const char *path,
                                   const char *interface, const char *property,
                                   sd_bus_message *reply, void *userdata,
                                   sd_bus_error *error) {
  struct state *s = userdata;
  return sd_bus_message_append(reply, "s", s->active);
}

static int prop_active_profile_set(sd_bus *bus, const char *path,
                                   const char *interface, const char *property,
                                   sd_bus_message *reply, void *userdata,
                                   sd_bus_error *error) {
  struct state *s = userdata;

  char *profile;
  int r = sd_bus_message_read(reply, "s", &profile);
  if (r < 0) {
    return log_error(r, "Failed to read dbus message:");
  }

  if (!config_tcc_profile(profile)) {
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "Unknown profile: %s", profile);
  }

  if (strcmp(profile, s->active) == 0)
    return 1;

  while (s->holds) {
    struct hold *h = s->holds;
    s->holds = h->next;
    emit_profile_released(bus, h);
    free_hold(h);
  }

  char *copy = strdup(profile);
  if (!copy)
    return -ENOMEM;
  free(s->base);
  s->base = copy;

  r = apply_profile(s, profile);
  if (r < 0)
    return r;

  sd_bus_emit_properties_changed(bus, path, interface, PPD_PROP_ACTIVE_PROFILE,
                                 PPD_PROP_ACTIVE_PROFILE_HOLDS, NULL);
  return 1;
}

// no tcc replacement, return empty string
static int prop_degraded_get(sd_bus *bus, const char *path,
                             const char *interface, const char *property,
                             sd_bus_message *reply, void *userdata,
                             sd_bus_error *error) {
  return sd_bus_message_append(reply, "s", "");
}

static int prop_profiles_get(sd_bus *bus, const char *path,
                             const char *interface, const char *property,
                             sd_bus_message *reply, void *userdata,
                             sd_bus_error *error) {
  static const char *const names[] = {"power-saver", "balanced", "performance"};
  int r;

  r = sd_bus_message_open_container(reply, 'a', "a{sv}");
  if (r < 0)
    return r;

  for (size_t i = 0; i < 3; i++) {
    r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0)
      return r;

    r = sd_bus_message_append(reply, "{sv}", "Profile", "s", names[i]);
    if (r < 0)
      return r;

    r = sd_bus_message_append(reply, "{sv}", "Driver", "s", "tcc");
    if (r < 0)
      return r;

    r = sd_bus_message_close_container(reply);
    if (r < 0)
      return r;
  }

  return sd_bus_message_close_container(reply);
}

static int prop_actions_get(sd_bus *bus, const char *path,
                            const char *interface, const char *property,
                            sd_bus_message *reply, void *userdata,
                            sd_bus_error *error) {
  int r = sd_bus_message_open_container(reply, 'a', "s");
  if (r < 0)
    return r;
  return sd_bus_message_close_container(reply);
}

static int prop_holds_get(sd_bus *bus, const char *path, const char *interface,
                          const char *property, sd_bus_message *reply,
                          void *userdata, sd_bus_error *error) {
  struct state *s = userdata;
  int r;

  r = sd_bus_message_open_container(reply, 'a', "a{sv}");
  if (r < 0)
    return r;

  for (struct hold *h = s->holds; h; h = h->next) {
    r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0)
      return r;

    r = sd_bus_message_append(reply, "{sv}", "ApplicationId", "s", h->app_id);
    if (r < 0)
      return r;
    r = sd_bus_message_append(reply, "{sv}", "Profile", "s", h->profile);
    if (r < 0)
      return r;
    r = sd_bus_message_append(reply, "{sv}", "Reason", "s", h->reason);
    if (r < 0)
      return r;

    r = sd_bus_message_close_container(reply);
    if (r < 0)
      return r;
  }

  return sd_bus_message_close_container(reply);
}

static int prop_version_get(sd_bus *bus, const char *path,
                            const char *interface, const char *property,
                            sd_bus_message *reply, void *userdata,
                            sd_bus_error *error) {
  return sd_bus_message_append(reply, "s", PPD_VERSION);
}

static const sd_bus_vtable ppd_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_METHOD_WITH_ARGS(
        PPD_METHOD_HOLD_PROFILE,
        SD_BUS_ARGS("s", profile, "s", reason, "s", application_id),
        SD_BUS_RESULT("u", cookie), method_hold_profile,
        SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_METHOD_WITH_ARGS(PPD_METHOD_RELEASE_PROFILE,
                            SD_BUS_ARGS("u", cookie), SD_BUS_NO_RESULT,
                            method_release_profile, SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_SIGNAL(PPD_SIGNAL_PROFILE_RELEASED, "u", 0),

    SD_BUS_WRITABLE_PROPERTY(PPD_PROP_ACTIVE_PROFILE, "s",
                             prop_active_profile_get, prop_active_profile_set,
                             0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

    SD_BUS_PROPERTY(PPD_PROP_PROFILES, "aa{sv}", prop_profiles_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_PROPERTY(PPD_PROP_ACTIVE_PROFILE_HOLDS, "aa{sv}", prop_holds_get, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

    SD_BUS_PROPERTY(PPD_PROP_PERFORMANCE_DEGRADED, "s", prop_degraded_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_PROPERTY(PPD_PROP_ACTIONS, "as", prop_actions_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_PROPERTY(PPD_PROP_VERSION, "s", prop_version_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),

    SD_BUS_VTABLE_END};

static struct state state;

int ppd_init(sd_bus *bus) {
  int r;
  r = init_state(&state);
  if (r < 0)
    return r;
  if (!state.active || !state.base)
    return -ENOMEM;

  r = sd_bus_add_object_vtable(bus, NULL, PPD_PATH, PPD_INTERFACE, ppd_vtable,
                               &state);
  if (r < 0)
    return log_error(r, "Failed to add vtable:");

  r = sd_bus_request_name(bus, PPD_INTERFACE, 0);
  if (r < 0)
    return log_error(r, "Failed to acquire name:");

  return 0;
}

void ppd_reapply(void) {
  int r = apply_profile(&state, state.active);
  if (r < 0)
    log_error(r, "Failed to reapply profile after config reload:");
}
