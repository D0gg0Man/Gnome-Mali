
# Push env to dbus so UI-launched apps get correct vars
sleep 2
XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 \
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
dbus-update-activation-environment --systemd \
  WAYLAND_DISPLAY XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS \
  XDG_SESSION_TYPE GDK_BACKEND

# Fix session type for UI-launched apps
sleep 3
XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 \
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
systemctl --user set-environment XDG_SESSION_TYPE=wayland XDG_SESSION_DESKTOP=gnome
