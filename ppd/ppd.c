#include "ppd.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../config/config.h"
#include "../tcc/tcc_client.h"

#define PPD_VERSION "0.30"

#define PPD_INTERFACE "org.freedesktop.UPower.PowerProfiles"
#define PPD_PATH "/org/freedesktop/UPower/PowerProfiles"
#define PPD_LEGACY_INTERFACE "net.hadess.PowerProfiles"
#define PPD_LEGACY_PATH "/net/hadess/PowerProfiles"

// polkit actions
#define POLKIT_ACTION_SWITCH_PROFILE "org.freedesktop.UPower.PowerProfiles.switch-profile"
#define POLKIT_ACTION_HOLD_PROFILE "org.freedesktop.UPower.PowerProfiles.hold-profile"

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

#define SYNC_GRACE_MS 2500ULL

struct state {
  char *active;
  char *base;
  uint32_t next_cookie;
  uint64_t suppress_sync_until_ms;
  struct hold *holds;
};

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void emit_properties_changed(sd_bus *bus, bool active_changed,
                                    bool holds_changed) {
  if (!bus || (!active_changed && !holds_changed))
    return;

  if (active_changed && holds_changed) {
    sd_bus_emit_properties_changed(bus, PPD_PATH, PPD_INTERFACE,
                                   PPD_PROP_ACTIVE_PROFILE,
                                   PPD_PROP_ACTIVE_PROFILE_HOLDS, NULL);
    sd_bus_emit_properties_changed(bus, PPD_LEGACY_PATH, PPD_LEGACY_INTERFACE,
                                   PPD_PROP_ACTIVE_PROFILE,
                                   PPD_PROP_ACTIVE_PROFILE_HOLDS, NULL);
  } else if (active_changed) {
    sd_bus_emit_properties_changed(bus, PPD_PATH, PPD_INTERFACE,
                                   PPD_PROP_ACTIVE_PROFILE, NULL);
    sd_bus_emit_properties_changed(bus, PPD_LEGACY_PATH, PPD_LEGACY_INTERFACE,
                                   PPD_PROP_ACTIVE_PROFILE, NULL);
  } else {
    sd_bus_emit_properties_changed(bus, PPD_PATH, PPD_INTERFACE,
                                   PPD_PROP_ACTIVE_PROFILE_HOLDS, NULL);
    sd_bus_emit_properties_changed(bus, PPD_LEGACY_PATH, PPD_LEGACY_INTERFACE,
                                   PPD_PROP_ACTIVE_PROFILE_HOLDS, NULL);
  }
}

static int apply_profile(struct state *s, const char *profile) {
  const char *tcc_name = config_tcc_profile(profile);
  if (!tcc_name)
    return -EINVAL;

  char *copy = strdup(profile);
  if (!copy)
    return -ENOMEM;

  int r = tcc_set_profile(tcc_name);
  if (r < 0) {
    free(copy);
    return r;
  }

  free(s->active);
  s->active = copy;
  s->suppress_sync_until_ms = monotonic_ms() + SYNC_GRACE_MS;
  return 0;
}

static int init_state(struct state *s) {
  const char *initial = "balanced"; // fallback
  bool needs_apply = true;
  char *tcc_active = NULL;

  int r = tcc_get_active_profile(&tcc_active);
  if (r >= 0 && tcc_active) {
    const char *mapped = config_ppd_profile(tcc_active);
    if (mapped) {
      initial = mapped;
      needs_apply = false;
    } else {
      fprintf(
          stderr,
          "Active TCC profile '%s' is not mapped in config, applying '%s'\n",
          tcc_active, initial);
    }
  } else {
    log_error(r, "Failed to get active TCC profile, assuming balanced");
  }
  free(tcc_active);

  if (needs_apply) {
    const char *tcc_name = config_tcc_profile(initial);
    if (tcc_name) {
      r = tcc_set_profile(tcc_name);
      if (r < 0)
        log_error(r, "Failed to apply initial fallback profile");
    }
  }

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

static void emit_profile_released_on(sd_bus *bus, const char *path,
                                     const char *interface, struct hold *h) {
  sd_bus_message *sig = NULL;
  int r = sd_bus_message_new_signal(bus, &sig, path, interface,
                                    PPD_SIGNAL_PROFILE_RELEASED);
  if (r < 0)
    return;

  r = sd_bus_message_set_destination(sig, h->sender);
  if (r >= 0)
    r = sd_bus_message_append(sig, "u", h->cookie);
  if (r >= 0)
    sd_bus_send(bus, sig, NULL);

  sd_bus_message_unref(sig);
}

static void emit_profile_released(sd_bus *bus, struct hold *h) {
  if (!bus || !h->sender)
    return;
  emit_profile_released_on(bus, PPD_PATH, PPD_INTERFACE, h);
  emit_profile_released_on(bus, PPD_LEGACY_PATH, PPD_LEGACY_INTERFACE, h);
}

static int check_authorized(sd_bus *bus, const char *action,
                            sd_bus_error *error) {
  sd_bus_message *msg = sd_bus_get_current_message(bus);
  const char *sender = msg ? sd_bus_message_get_sender(msg) : NULL;
  if (!sender)
    return sd_bus_error_set(error, SD_BUS_ERROR_ACCESS_DENIED, "No sender");

  // root: allow directly
  sd_bus_creds *creds = NULL;
  int r = sd_bus_query_sender_creds(msg, SD_BUS_CREDS_UID, &creds);
  if (r >= 0) {
    uid_t uid;
    int ok = sd_bus_creds_get_uid(creds, &uid);
    sd_bus_creds_unref(creds);
    if (ok >= 0 && uid == 0)
      return 0;
  }

  sd_bus_message *reply = NULL;
  r = sd_bus_call_method(bus, "org.freedesktop.PolicyKit1",
                         "/org/freedesktop/PolicyKit1/Authority",
                         "org.freedesktop.PolicyKit1.Authority",
                         "CheckAuthorization", NULL, &reply,
                         "(sa{sv})sa{ss}us",
                         "system-bus-name", 1, "name", "s", sender,
                         action, 0, 0 /* non-interactive */, "");
  if (r < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED,
                             "Polkit check failed: %s", strerror(-r));

  int authorized = 0, challenge = 0;
  r = sd_bus_message_enter_container(reply, 'r', "bba{ss}");
  if (r >= 0)
    r = sd_bus_message_read(reply, "bb", &authorized, &challenge);
  sd_bus_message_unref(reply);

  if (r < 0 || !authorized)
    return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED,
                             "Not authorized: %s", action);
  return 0;
}

static int method_hold_profile(sd_bus_message *m, void *userdata,
                               sd_bus_error *error) {
  struct state *s = userdata;
  const char *profile, *reason, *app_id;

  int r = check_authorized(sd_bus_message_get_bus(m), POLKIT_ACTION_HOLD_PROFILE,
                           error);
  if (r < 0)
    return r;

  r = sd_bus_message_read(m, "sss", &profile, &reason, &app_id);
  if (r < 0) {
    return log_error(r, "Failed to read dbus message");
  }

  // ppd only allows holding these two
  if (strcmp(profile, "power-saver") != 0 &&
      strcmp(profile, "performance") != 0)
    return sd_bus_error_setf(
        error, SD_BUS_ERROR_INVALID_ARGS,
        "Only 'power-saver' and 'performance' can be held");

  const char *sender = sd_bus_message_get_sender(m);
  if (!sender)
    return -EINVAL;

  struct hold *h = calloc(1, sizeof(*h));
  if (!h)
    return -ENOMEM;

  h->cookie = ++s->next_cookie;
  h->profile = strdup(profile);
  h->reason = strdup(reason);
  h->app_id = strdup(app_id);
  h->sender = strdup(sender);
  if (!h->profile || !h->reason || !h->app_id || !h->sender) {
    free_hold(h);
    return -ENOMEM;
  }

  h->next = s->holds;
  s->holds = h;
  const char *target = effective_hold_profile(s);
  bool profile_changed = strcmp(target, s->active) != 0;
  r = apply_profile(s, target);
  if (r < 0) {
    s->holds = h->next;
    free_hold(h);
    return log_error(r, "Failed to apply held profile");
  }

  sd_bus *bus = sd_bus_message_get_bus(m);
  if (!bus) {
    s->holds = h->next;
    free_hold(h);
    return -EINVAL;
  }
  emit_properties_changed(bus, profile_changed, true);

  return sd_bus_reply_method_return(m, "u", h->cookie);
}

static int method_release_profile(sd_bus_message *m, void *userdata,
                                  sd_bus_error *error) {
  struct state *s = userdata;
  uint32_t cookie;

  int r = sd_bus_message_read(m, "u", &cookie);
  if (r < 0) {
    return log_error(r, "Failed to read dbus message");
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
    bool active_changed = strcmp(target, s->active) != 0;
    r = apply_profile(s, target);
    if (r < 0) {
      log_error(r, "Failed to apply profile after release");
      active_changed = false;
    }

    sd_bus *bus = sd_bus_message_get_bus(m);
    if (!bus)
      return -EINVAL;
    emit_properties_changed(bus, active_changed, true);

    return sd_bus_reply_method_return(m, "");
  }
  return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "No such hold: %u",
                           cookie);
}

static int on_name_owner_changed(sd_bus_message *m, void *userdata,
                                 sd_bus_error *error) {
  (void)error;
  struct state *s = userdata;
  const char *name, *old_owner, *new_owner;

  int r = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
  if (r < 0 || strcmp(name, old_owner) != 0 || (new_owner && *new_owner))
    return 0;

  bool removed = false;
  for (struct hold **p = &s->holds; *p;) {
    if (strcmp((*p)->sender, old_owner) == 0) {
      struct hold *h = *p;
      *p = h->next;
      free_hold(h);
      removed = true;
    } else {
      p = &(*p)->next;
    }
  }

  if (!removed)
    return 0;

  const char *target = effective_hold_profile(s);
  if (!target)
    target = s->base;

  bool active_changed = strcmp(target, s->active) != 0;
  r = apply_profile(s, target);
  if (r < 0) {
    log_error(r, "Failed to apply profile after hold owner exited");
    active_changed = false;
  }

  emit_properties_changed(sd_bus_message_get_bus(m), active_changed, true);
  return 0;
}

static int prop_active_profile_get(sd_bus *bus, const char *path,
                                   const char *interface, const char *property,
                                   sd_bus_message *reply, void *userdata,
                                   sd_bus_error *error) {
  (void)bus;
  (void)path;
  (void)interface;
  (void)property;
  (void)error;
  struct state *s = userdata;
  return sd_bus_message_append(reply, "s", s->active);
}

static int prop_active_profile_set(sd_bus *bus, const char *path,
                                   const char *interface, const char *property,
                                   sd_bus_message *reply, void *userdata,
                                   sd_bus_error *error) {
  (void)path;
  (void)interface;
  (void)property;
  struct state *s = userdata;

  int r = check_authorized(bus, POLKIT_ACTION_SWITCH_PROFILE, error);
  if (r < 0)
    return r;

  const char *profile;
  r = sd_bus_message_read(reply, "s", &profile);
  if (r < 0) {
    return log_error(r, "Failed to read dbus message");
  }

  if (!config_tcc_profile(profile)) {
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "Unknown profile: %s", profile);
  }

  if (!s->holds && strcmp(profile, s->active) == 0)
    return 1;

  char *copy = strdup(profile);
  if (!copy)
    return -ENOMEM;

  bool active_changed = strcmp(profile, s->active) != 0;
  bool holds_changed = s->holds != NULL;

  if (active_changed) {
    r = apply_profile(s, profile);
    if (r < 0) {
      free(copy);
      return r;
    }
  }

  while (s->holds) {
    struct hold *h = s->holds;
    s->holds = h->next;
    emit_profile_released(bus, h);
    free_hold(h);
  }

  free(s->base);
  s->base = copy;

  emit_properties_changed(bus, active_changed, holds_changed);
  return 1;
}

// no tcc replacement, return empty string
static int prop_degraded_get(sd_bus *bus, const char *path,
                             const char *interface, const char *property,
                             sd_bus_message *reply, void *userdata,
                             sd_bus_error *error) {
  (void)bus;
  (void)path;
  (void)interface;
  (void)property;
  (void)userdata;
  (void)error;
  return sd_bus_message_append(reply, "s", "");
}

static int prop_profiles_get(sd_bus *bus, const char *path,
                             const char *interface, const char *property,
                             sd_bus_message *reply, void *userdata,
                             sd_bus_error *error) {
  (void)bus;
  (void)path;
  (void)interface;
  (void)property;
  (void)userdata;
  (void)error;
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
  (void)bus;
  (void)path;
  (void)interface;
  (void)property;
  (void)userdata;
  (void)error;
  int r = sd_bus_message_open_container(reply, 'a', "s");
  if (r < 0)
    return r;
  return sd_bus_message_close_container(reply);
}

static int prop_holds_get(sd_bus *bus, const char *path, const char *interface,
                          const char *property, sd_bus_message *reply,
                          void *userdata, sd_bus_error *error) {
  (void)bus;
  (void)path;
  (void)interface;
  (void)property;
  (void)error;
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
  (void)bus;
  (void)path;
  (void)interface;
  (void)property;
  (void)userdata;
  (void)error;
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

    SD_BUS_WRITABLE_PROPERTY(
        PPD_PROP_ACTIVE_PROFILE, "s", prop_active_profile_get,
        prop_active_profile_set, 0,
        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE | SD_BUS_VTABLE_UNPRIVILEGED),

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
    return log_error(r, "Failed to add UPower vtable");

  r = sd_bus_add_object_vtable(bus, NULL, PPD_LEGACY_PATH, PPD_LEGACY_INTERFACE,
                               ppd_vtable, &state);
  if (r < 0)
    return log_error(r, "Failed to add legacy vtable");

  r = sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus",
                          "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "NameOwnerChanged", on_name_owner_changed, &state);
  if (r < 0)
    return log_error(r, "Failed to watch NameOwnerChanged");

  r = sd_bus_request_name(bus, PPD_INTERFACE, 0);
  if (r < 0)
    return log_error(r, "Failed to acquire " PPD_INTERFACE);

  r = sd_bus_request_name(bus, PPD_LEGACY_INTERFACE, 0);
  if (r < 0)
    return log_error(r, "Failed to acquire " PPD_LEGACY_INTERFACE);

  return 0;
}

void ppd_reapply(void) {
  int r = apply_profile(&state, state.active);
  if (r < 0)
    log_error(r, "Failed to reapply profile after config reload");
}

void ppd_sync(sd_bus *bus) {
  if (!bus || !state.active)
    return;
  if (monotonic_ms() < state.suppress_sync_until_ms)
    return;

  char *tcc_active = NULL;
  if (tcc_get_active_profile(&tcc_active) < 0 || !tcc_active) {
    free(tcc_active);
    return;
  }

  const char *mapped = config_ppd_profile(tcc_active);
  free(tcc_active);
  if (!mapped || strcmp(mapped, state.active) == 0)
    return;

  char *new_active = strdup(mapped);
  char *new_base = strdup(mapped);
  if (!new_active || !new_base) {
    free(new_active);
    free(new_base);
    return;
  }

  bool holds_changed = state.holds != NULL;
  while (state.holds) {
    struct hold *h = state.holds;
    state.holds = h->next;
    emit_profile_released(bus, h);
    free_hold(h);
  }

  free(state.active);
  free(state.base);
  state.active = new_active;
  state.base = new_base;

  emit_properties_changed(bus, true, holds_changed);
}

void ppd_cleanup(void) {
  while (state.holds) {
    struct hold *h = state.holds;
    state.holds = h->next;
    free_hold(h);
  }
  free(state.active);
  state.active = NULL;
  free(state.base);
  state.base = NULL;
}
