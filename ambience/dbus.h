#pragma once

#include <systemd/sd-bus.h>

typedef void (*ambience_next_cb)(void *);

struct AmbienceDbus;

// Hosts io.homeboard.Ambience on the given bus (borrowed; owned by the caller
// and must outlive the AmbienceDbus). `on_next` is invoked from the bus
// dispatch loop when Next() is called.
struct AmbienceDbus *ambience_dbus_init(sd_bus *bus, ambience_next_cb on_next, void *ud);
void ambience_dbus_free(struct AmbienceDbus *d);

// Process pending messages and block until new activity (or a signal).
// Returns negative on error, 0 otherwise.
int ambience_dbus_run_once(struct AmbienceDbus *d);
