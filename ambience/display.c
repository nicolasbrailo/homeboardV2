#include "display.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

#define DBUS_OCCUPANCY_SERVICE "io.homeboard.Occupancy"
#define DBUS_OCCUPANCY_PATH "/io/homeboard/Occupancy"
#define DBUS_OCCUPANCY_INTERFACE "io.homeboard.Occupancy1"
#define DBUS_OCCUPANCY_SIGNAL "StateChanged"

#define DBUS_DISPLAY_SERVICE "io.homeboard.Display"
#define DBUS_DISPLAY_PATH "/io/homeboard/Display"
#define DBUS_DISPLAY_INTERFACE "io.homeboard.Display1"

struct Display {
  sd_bus *dbus;
  sd_bus_slot *occupancy_monitor;
  sd_bus_slot *occupancy_name_owner_monitor;

  display_state_cb on_display_turned_on;
  display_state_cb on_display_turned_off;
  void *ud;

  struct OccupancyReport {
    bool occupied;
    int distance;
  } last_occupancy_report;

  int requested_state;
};

static void update_display_state(struct Display *display) {
  if (display->last_occupancy_report.occupied && display->requested_state == 1) {
    // Do nothing, display already in expected state
    return;
  } else if (!display->last_occupancy_report.occupied && display->requested_state == 0) {
    // Do nothing, display already in expected state
    return;
  }

  // Display in unknown state, or state doesn't match presence status
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const char *method = display->last_occupancy_report.occupied == true ? "On" : "Off";
  const int r = sd_bus_call_method(display->dbus, DBUS_DISPLAY_SERVICE, DBUS_DISPLAY_PATH, DBUS_DISPLAY_INTERFACE, method, &err,
                         &reply, "");
  if (r < 0) {
    fprintf(stderr, "Failed to call Display.%s: %s\n", method, err.message ? err.message : strerror(-r));
    display->requested_state = -1;
  } else {
    printf("Occupancy state changed, calling Display.%s()\n", method);
    display->requested_state = display->last_occupancy_report.occupied ? 1 : 0;
    display_state_cb cb =
        display->last_occupancy_report.occupied ? display->on_display_turned_on : display->on_display_turned_off;
    cb(display->ud);
  }

  sd_bus_error_free(&err);
  sd_bus_message_unref(reply);
}

static int on_occupancy_state_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  struct Display *display = userdata;
  display->last_occupancy_report.occupied = 0;
  display->last_occupancy_report.distance = 0;
  const int r = sd_bus_message_read(m, "bu", &display->last_occupancy_report.occupied, &display->last_occupancy_report.distance);
  if (r < 0) {
    fprintf(stderr, "sd_bus_message_read: %s\n", strerror(-r));
    return 0;
  }

  update_display_state(display);
  // TODO: Add some hysteresis to occupancy service?
  return 0;
}

// One-shot check: does anyone currently own the occupancy service name?
static void log_occupancy_presence(sd_bus *bus) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const char *owner = NULL;
  int r = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                             "GetNameOwner", &err, &reply, "s", DBUS_OCCUPANCY_SERVICE);
  if (r < 0) {
    if (sd_bus_error_has_name(&err, "org.freedesktop.DBus.Error.NameHasNoOwner"))
      fprintf(stderr, "WARNING: %s is not running; no occupancy signals will arrive until it starts\n",
              DBUS_OCCUPANCY_SERVICE);
    else
      fprintf(stderr, "GetNameOwner(%s) failed: %s\n", DBUS_OCCUPANCY_SERVICE,
              err.message ? err.message : strerror(-r));
  } else {
    r = sd_bus_message_read(reply, "s", &owner);
    if (r >= 0)
      printf("Occupancy service %s offered by %s\n", DBUS_OCCUPANCY_SERVICE, owner);
  }
  sd_bus_error_free(&err);
  sd_bus_message_unref(reply);
}

static int on_occupancy_name_owner_changed(sd_bus_message *m, void* ud, sd_bus_error*) {
  const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
  int r = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
  if (r < 0)
    return 0;
  if (new_owner && new_owner[0] == '\0') {
    fprintf(stderr, "WARNING: %s disappeared from the bus\n", name);
    // Reset tracked state
    struct Display *display = ud;
    display->last_occupancy_report.occupied = false;
    display->last_occupancy_report.distance = 0;
    update_display_state(display);
  }

  else if (old_owner && old_owner[0] == '\0')
    printf("%s appeared on the bus (owner=%s)\n", name, new_owner);
  return 0;
}

struct Display *display_init(display_state_cb on_display_turned_on, display_state_cb on_display_turned_off, void *ud) {
  struct Display *s = calloc(1, sizeof(*s));
  if (!s) {
    return NULL;
  }

  s->on_display_turned_on = on_display_turned_on;
  s->on_display_turned_off = on_display_turned_off;
  s->ud = ud;

  int r = sd_bus_open_system(&s->dbus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    display_free(s);
    return NULL;
  }

  r = sd_bus_match_signal(s->dbus, &s->occupancy_monitor, DBUS_OCCUPANCY_SERVICE, DBUS_OCCUPANCY_PATH,
                          DBUS_OCCUPANCY_INTERFACE, DBUS_OCCUPANCY_SIGNAL, on_occupancy_state_changed, s);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal: %s\n", strerror(-r));
    display_free(s);
    return NULL;
  }

  // Track ownership of the occupancy service so we can log when it comes or
  // goes. The arg0 filter narrows the signal to just that name.
  char match[256];
  snprintf(match, sizeof(match),
           "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
           "member='NameOwnerChanged',arg0='%s'",
           DBUS_OCCUPANCY_SERVICE);
  r = sd_bus_add_match(s->dbus, &s->occupancy_name_owner_monitor, match, on_occupancy_name_owner_changed, s);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_match NameOwnerChanged: %s\n", strerror(-r));
    display_free(s);
    return NULL;
  }

  log_occupancy_presence(s->dbus);

  printf("Display controller now monitoring occupancy signals %s.%s\n", DBUS_OCCUPANCY_INTERFACE, DBUS_OCCUPANCY_SIGNAL);
  return s;
}

void display_free(struct Display *s) {
  if (!s)
    return;
  if (s->occupancy_monitor)
    sd_bus_slot_unref(s->occupancy_monitor);
  if (s->occupancy_name_owner_monitor)
    sd_bus_slot_unref(s->occupancy_name_owner_monitor);
  if (s->dbus)
    sd_bus_flush_close_unref(s->dbus);
}

int display_run_dbus_loop(struct Display *s) {
  while (true) {
    int r = sd_bus_process(s->dbus, NULL);
    if (r < 0) {
      fprintf(stderr, "sd_bus_process: %s\n", strerror(-r));
      return r;
    }
    if (r > 0)
      continue;

    r = sd_bus_wait(s->dbus, (uint64_t)-1);
    if (r < 0 && -r != EINTR) {
      fprintf(stderr, "sd_bus_wait: %s\n", strerror(-r));
    }
    return r;
  }
}
