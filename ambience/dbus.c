#include "dbus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define DBUS_SERVICE "io.homeboard.Ambience"
#define DBUS_PATH "/io/homeboard/Ambience"
#define DBUS_INTERFACE "io.homeboard.Ambience1"

struct AmbienceDbus {
  sd_bus *bus;
  sd_bus_slot *vtable_slot;
  ambience_next_cb on_next;
  void *ud;
};

static int method_next(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct AmbienceDbus *d = userdata;
  d->on_next(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable g_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", method_next, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

struct AmbienceDbus *ambience_dbus_init(ambience_next_cb on_next, void *ud) {
  struct AmbienceDbus *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->on_next = on_next;
  d->ud = ud;

  int r = sd_bus_open_system(&d->bus);
  if (r < 0) {
    fprintf(stderr, "ambience_dbus: sd_bus_open_system: %s\n", strerror(-r));
    free(d);
    return NULL;
  }
  r = sd_bus_add_object_vtable(d->bus, &d->vtable_slot, DBUS_PATH, DBUS_INTERFACE, g_vtable, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    ambience_dbus_free(d);
    return NULL;
  }
  r = sd_bus_request_name(d->bus, DBUS_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_SERVICE, strerror(-r));
    ambience_dbus_free(d);
    return NULL;
  }
  printf("Ambience service offered at %s\n", DBUS_SERVICE);
  return d;
}

void ambience_dbus_free(struct AmbienceDbus *d) {
  if (!d)
    return;
  if (d->vtable_slot)
    sd_bus_slot_unref(d->vtable_slot);
  if (d->bus)
    sd_bus_flush_close_unref(d->bus);
  free(d);
}

sd_bus *ambience_dbus_get_bus(struct AmbienceDbus *d) { return d->bus; }

int ambience_dbus_run_once(struct AmbienceDbus *d) {
  int r = sd_bus_process(d->bus, NULL);
  if (r < 0) {
    fprintf(stderr, "sd_bus_process: %s\n", strerror(-r));
    return r;
  }
  if (r > 0)
    return 0;
  r = sd_bus_wait(d->bus, (uint64_t)-1);
  if (r < 0 && -r != EINTR) {
    fprintf(stderr, "sd_bus_wait: %s\n", strerror(-r));
    return r;
  }
  return 0;
}
