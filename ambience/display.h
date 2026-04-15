#pragma once

struct Display;

typedef void (*display_state_cb)(void *);

struct Display *display_init(display_state_cb on_display_turned_on, display_state_cb on_display_turned_off, void *ud);
void display_free(struct Display *s);
int display_run_dbus_loop(struct Display *s);
