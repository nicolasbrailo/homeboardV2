#pragma once

#include <systemd/sd-bus.h>

typedef void (*ambience_next_cb)(void *);

struct AmbienceDbus;

// Opens a system-bus connection and hosts io.homeboard.Ambience on it. The
// bus is shared with other modules (see ambience_dbus_get_bus) so the whole
// process runs a single sd_bus loop. `on_next` is invoked from that loop when
// Next() is called.
struct AmbienceDbus *ambience_dbus_init(ambience_next_cb on_next, void *ud);
void ambience_dbus_free(struct AmbienceDbus *d);

// Borrowed bus pointer. Valid for the lifetime of the AmbienceDbus.
sd_bus *ambience_dbus_get_bus(struct AmbienceDbus *d);

// Process pending messages and block until new activity (or a signal).
// Returns negative on error, 0 otherwise.
int ambience_dbus_run_once(struct AmbienceDbus *d);
