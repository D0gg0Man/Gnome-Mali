#!/bin/bash
set -e

MALI_DIR="$HOME/gnome-mali/built"
EGL_LIB="/usr/lib/aarch64-linux-gnu/libEGL_hybris_wrapper.so"
GBM_LIB="/usr/lib/aarch64-linux-gnu/gbm/hybris_gbm.so"
USER_ENV="$HOME/.config/environment.d/gnome-mali.conf"

switch_gnome() {
    echo "[0/9] Checking system libs..."
    if [ ! -e /usr/lib/aarch64-linux-gnu/libvulkan_libhybris.so ]; then
        echo "Fixing broken libvulkan.so.1 symlink..."
        sudo ln -sf /usr/lib/aarch64-linux-gnu/libvulkan-hybris.so /usr/lib/aarch64-linux-gnu/libvulkan_libhybris.so
    fi

    echo "[0/9] Killing Android composer first..."
    sudo setprop ctl.stop vendor.hwcomposer-2-3 2>/dev/null || true
    sudo setprop ctl.stop vendor.pq 2>/dev/null || true
    for i in $(seq 1 8); do
        sudo kill -9 $(sudo cat /sys/kernel/debug/dri/0/clients 2>/dev/null | grep -E "composer|pq@" | awk '{print $2}') 2>/dev/null || true
        sleep 0.3
    done


    echo "[0b/9] Setting up GNOME Mali session..."
    if [ ! -f /usr/share/wayland-sessions/gnome-mali.desktop ]; then
        sudo cp $HOME/gnome-mali/gnome-mali-session /usr/local/bin/gnome-mali-session
        sudo chmod +x /usr/local/bin/gnome-mali-session
        sudo tee /usr/share/wayland-sessions/gnome-mali.desktop > /dev/null << DEOF
[Desktop Entry]
Name=GNOME Mali
Comment=GNOME Shell on Mali GPU via HWC2
Exec=/usr/local/bin/gnome-mali-session
TryExec=/usr/local/bin/gnome-mali-session
Type=Application
DesktopNames=GNOME
X-GDM-SessionRegisters=true
DEOF
        echo "Created GNOME Mali wayland session"
    fi
    echo "[1/9] Stopping phosh..."
    sudo systemctl stop phosh 2>/dev/null || true
    sudo systemctl mask phosh 2>/dev/null || true
    sudo pkill -9 phosh 2>/dev/null || true
    sudo chvt 3
    sleep 1

    echo "[2/9] Installing GNOME Mali libs..."
    sudo cp $MALI_DIR/libEGL_hybris_wrapper_gnome.so $EGL_LIB
    sudo cp /home/furios/gnome-mali/built/05_hybris_wrapper.json /usr/share/glvnd/egl_vendor.d/05_hybris_wrapper.json
    sudo rm -f /usr/share/glvnd/egl_vendor.d/10_libhybris.json
    sudo cp $MALI_DIR/hybris_gbm.so $GBM_LIB
    sudo cp $MALI_DIR/drm_shim.so /tmp/drm_shim_new.so && sudo mv /tmp/drm_shim_new.so /usr/local/lib/drm_shim.so
    sudo cp $MALI_DIR/wlegl_server.so /tmp/wlegl_new.so && sudo mv /tmp/wlegl_new.so /usr/local/lib/wlegl_server.so

    echo "[3/9] Setting user environment..."
    mkdir -p "$(dirname $USER_ENV)"
    cat > $USER_ENV << 'ENV'
GSK_RENDERER=gl
GDK_BACKEND=wayland
GDK_GL=gles
XDG_SESSION_TYPE=wayland
XDG_SESSION_DESKTOP=gnome
ENV

    echo "[4/9] Cleaning up old session..."
    sudo systemctl stop gnome-test 2>/dev/null || true
    sudo systemctl reset-failed gnome-test 2>/dev/null || true
    sudo rm -f /tmp/gnome.log /run/user/32011/wayland-*
    sudo pkill -9 gnome-shell mutter gjs 2>/dev/null || true
    sleep 1

    echo "[4b/9] Starting Android composer for HWC2..."
    sudo setprop ctl.start vendor.hwcomposer-2-3 2>/dev/null || true
    sleep 3

    echo "[5/9] Starting GNOME Shell..."
    sudo systemd-run --unit=gnome-test --uid=furios \
        --setenv=XDG_RUNTIME_DIR=/run/user/32011 \
        --setenv=HOME=/home/furios \
        --setenv=XDG_SEAT=seat0 \
        --setenv=XDG_VTNR=3 \
        --setenv=XDG_SESSION_TYPE=wayland \
        --setenv=XDG_SESSION_CLASS=user \
        --setenv=GBM_BACKEND=hybris \
        --setenv=GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm \
        "--setenv=LD_PRELOAD=/usr/local/lib/drm_shim.so /usr/local/lib/wlegl_server.so" \
        --setenv=GSK_RENDERER=gl \
        --setenv=GDK_BACKEND=wayland \
        --setenv=XDG_CURRENT_DESKTOP=GNOME \
        -p PAMName=gdm-wayland-session \
        -p TTYPath=/dev/tty3 \
        -p StandardOutput=file:/tmp/gnome.log \
        -p StandardError=file:/tmp/gnome.log \
        -- gnome-shell --wayland --no-x11

    echo "[6/9] Waiting for shell to start..."
    sleep 12

    echo "[7/9] Activating session and fixing env..."
    SESSION=$(loginctl list-sessions | grep seat0 | grep furios | awk '{print $1}' | head -1)
    sudo loginctl activate $SESSION 2>/dev/null || true

    XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
    systemctl --user set-environment \
        XDG_SESSION_TYPE=wayland \
        XDG_SESSION_DESKTOP=gnome \
        GDK_GL=gles \
        GSK_RENDERER=gl \
        GDK_BACKEND=wayland \

    echo "[8/9] Starting GNOME settings daemons..."
    XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0     DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus     sudo -u furios /usr/libexec/gsd-media-keys &
    XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0     DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus     sudo -u furios /usr/libexec/gsd-power &
    sleep 2

    echo "GNOME Mali is running! Check /tmp/gnome.log for details."
}


switch_phosh() {
    echo "[1/5] Stopping GNOME..."
    sudo systemctl stop gnome-test 2>/dev/null || true
    sudo systemctl reset-failed gnome-test 2>/dev/null || true
    sudo pkill -9 gnome-shell mutter gjs 2>/dev/null || true
    sleep 1

    echo "[2/5] Restoring original Phosh libs..."
    sudo cp $MALI_DIR/libEGL_hybris_wrapper_phosh.so $EGL_LIB
    sudo rm -f /usr/share/glvnd/egl_vendor.d/05_hybris_wrapper.json

    echo "[3/5] Removing GNOME user env overrides..."
    sudo tee /usr/share/glvnd/egl_vendor.d/10_libhybris.json << 'EOF'
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "libEGL_libhybris.so.0"
    }
}
EOF
    XDG_RUNTIME_DIR=/run/user/32011 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus systemctl --user unset-environment GSK_RENDERER GDK_BACKEND GDK_GL XDG_SESSION_DESKTOP VK_ICD_FILENAMES 2>/dev/null || true
    rm -f $USER_ENV

    echo "[4/5] Starting Phosh..."
    sudo systemctl unmask phosh 2>/dev/null || true
    sudo setprop ctl.start vendor.hwcomposer-2-3 2>/dev/null || true
    sleep 3
    sudo systemctl start phosh

    echo "[5/5] Done!"
    echo "Phosh is running!"
}

status() {
    echo "=== GNOME Mali Status ==="
    echo "gnome-test: $(systemctl is-active gnome-test 2>/dev/null || echo inactive)"
    echo "phosh:      $(systemctl is-active phosh 2>/dev/null || echo inactive)"
    echo ""
    echo "=== Installed Libs ==="
    md5sum $EGL_LIB $MALI_DIR/libEGL_hybris_wrapper.so $MALI_DIR/libEGL_hybris_wrapper_phosh.so 2>/dev/null
}

case "$1" in
    gnome)  switch_gnome ;;
    phosh)  switch_phosh ;;
    status) status ;;
    *)
        echo "Usage: $0 [gnome|phosh|status]"
        status
        ;;
esac
