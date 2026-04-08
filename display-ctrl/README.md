# display-ctrl

CLI client for `display-mgr`. Sends a command over the Unix domain socket and prints the response.

```
display-ctrl on|off|status
```

Requires `display-mgr` to be running. Connects to `$XDG_RUNTIME_DIR/display-ctrl.sock`.
