#!/bin/bash
# =============================================================================
# GNOME Mali Session Installer for FuriOS (MT6877 / Dimensity 900)
# Installs GNOME Shell alongside Phosh via the phrog greeter
#
# Usage: sudo ./install-gnome-mali.sh
#
# Required files (same directory as this script):
#   libEGL_libhybris.so.0.0.0   — patched libhybris EGL (GBM→drmadapter routing)
#   eglplatform_drmadapter.so   — hybris EGL platform (HWC2 init + present)
#   drm_shim.so                 — DRM ioctl interceptor for mutter KMS
#   wlegl_server.so             — Wayland EGL server
#   vulkan_x11_stub.so          — stub for missing X11 Vulkan symbols in GTK4
# =============================================================================

set -e

INSTALL_DIR="$(dirname "$(readlink -f "$0")")"
EGL_LIB_DIR="/usr/lib/aarch64-linux-gnu"
HYBRIS_PLATFORM_DIR="/usr/lib/aarch64-linux-gnu/libhybris"
BACKUP_DIR="/var/lib/gnome-mali/backups"
STATE_FILE="/var/lib/gnome-mali/installed"

echo "=== GNOME Mali Session Installer ==="
echo "Install source: $INSTALL_DIR"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Please run as root (sudo $0)"
    exit 1
fi

# -----------------------------------------------------------------------------
# Verify required files
# -----------------------------------------------------------------------------
REQUIRED=(
    libEGL_libhybris.so.0.0.0
    eglplatform_drmadapter.so
    drm_shim.so
    wlegl_server.so
    vulkan_x11_stub.so
)

for f in "${REQUIRED[@]}"; do
    if [ ! -f "$INSTALL_DIR/$f" ]; then
        echo "ERROR: Missing required file: $INSTALL_DIR/$f"
        exit 1
    fi
done

# -----------------------------------------------------------------------------
# Backup helper — only backs up once (won't overwrite existing backup)
# -----------------------------------------------------------------------------
mkdir -p "$BACKUP_DIR"

backup() {
    local src="$1"
    local dest="$BACKUP_DIR/$(echo "$src" | tr '/' '_')"
    if [ -f "$src" ] && [ ! -f "$dest" ]; then
        cp -a "$src" "$dest"
        echo "  backed up: $src -> $dest"
    fi
}

# -----------------------------------------------------------------------------
# Step 1 — Patched libhybris EGL
# Adds EGL_PLATFORM_GBM_KHR routing to drmadapter platform,
# and GBM pixel format fix in eglGetConfigAttrib.
# -----------------------------------------------------------------------------
echo "[1/7] Installing patched libhybris EGL..."
backup "$EGL_LIB_DIR/libEGL_libhybris.so.0.0.0"

cp "$INSTALL_DIR/libEGL_libhybris.so.0.0.0" /tmp/_libhybris_egl.so
mv /tmp/_libhybris_egl.so "$EGL_LIB_DIR/libEGL_libhybris.so.0.0.0"
chown root:root "$EGL_LIB_DIR/libEGL_libhybris.so.0.0.0"
chmod 755 "$EGL_LIB_DIR/libEGL_libhybris.so.0.0.0"
ldconfig

# -----------------------------------------------------------------------------
# Step 2 — drmadapter hybris EGL platform + shim libraries
# -----------------------------------------------------------------------------
echo "[2/7] Installing drmadapter platform and shim libraries..."
backup /usr/local/lib/drm_shim.so
backup /usr/local/lib/wlegl_server.so
backup /usr/local/lib/vulkan_x11_stub.so

mkdir -p "$HYBRIS_PLATFORM_DIR"
cp "$INSTALL_DIR/eglplatform_drmadapter.so" /tmp/_drmadapter.so
mv /tmp/_drmadapter.so "$HYBRIS_PLATFORM_DIR/eglplatform_drmadapter.so"
chown root:root "$HYBRIS_PLATFORM_DIR/eglplatform_drmadapter.so"
chmod 755 "$HYBRIS_PLATFORM_DIR/eglplatform_drmadapter.so"

cp "$INSTALL_DIR/drm_shim.so"       /tmp/_drm_shim.so
mv /tmp/_drm_shim.so    /usr/local/lib/drm_shim.so
cp "$INSTALL_DIR/wlegl_server.so"   /tmp/_wlegl.so
mv /tmp/_wlegl.so       /usr/local/lib/wlegl_server.so
cp "$INSTALL_DIR/vulkan_x11_stub.so" /tmp/_vk_stub.so
mv /tmp/_vk_stub.so     /usr/local/lib/vulkan_x11_stub.so

# -----------------------------------------------------------------------------
# Step 3 — /etc/ld.so.preload
# drm_shim and wlegl_server must preload into all processes
# -----------------------------------------------------------------------------
echo "[3/7] Configuring ld.so.preload..."
backup /etc/ld.so.preload

touch /etc/ld.so.preload
grep -qxF '/usr/local/lib/drm_shim.so'    /etc/ld.so.preload || \
    echo '/usr/local/lib/drm_shim.so'    >> /etc/ld.so.preload
grep -qxF '/usr/local/lib/wlegl_server.so' /etc/ld.so.preload || \
    echo '/usr/local/lib/wlegl_server.so' >> /etc/ld.so.preload
grep -qxF '/usr/local/lib/vulkan_x11_stub.so' /etc/ld.so.preload || \
    echo '/usr/local/lib/vulkan_x11_stub.so' >> /etc/ld.so.preload

# -----------------------------------------------------------------------------
# Step 4 — Session wrapper
# No vendor swap needed — libhybris routes GBM to drmadapter natively
# -----------------------------------------------------------------------------
echo "[4/7] Installing session wrapper..."
backup /usr/libexec/gnome-mali-session

cat > /usr/libexec/gnome-mali-session << 'SCRIPT'
#!/bin/bash
# GNOME Mali session launcher — called by greetd via gnome-mali.desktop
#
# Pipeline:
#   mutter → libEGL_libhybris.so (patched) → eglplatform_drmadapter.so → HWC2
#
# No GLVND vendor wrapper or vendor swap needed.
# libhybris routes EGL_PLATFORM_GBM_KHR directly to drmadapter platform.

export GBM_BACKEND=hybris
export GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm
export GSK_RENDERER=gl
export GDK_BACKEND=wayland
export GDK_GL=gles
export XDG_CURRENT_DESKTOP=GNOME
export XDG_SESSION_DESKTOP=gnome
export MUTTER_DEBUG_FORCE_KMS_MODE=simple
export HYBRIS_EGLPLATFORM=drmadapter
unset WLR_BACKENDS WLR_HWC_SKIP_VERSION_CHECK EGL_PLATFORM

# Start gsd-media-keys once wayland socket is ready
(
    for i in $(seq 1 40); do
        [ -S "${XDG_RUNTIME_DIR:-/run/user/32011}/wayland-0" ] && break
        sleep 0.5
    done
    export WAYLAND_DISPLAY=wayland-0
    /usr/libexec/gsd-media-keys 2>/dev/null &
) &

exec env -u __EGL_VENDOR_LIBRARY_FILENAMES \
    LD_PRELOAD="/usr/local/lib/drm_shim.so /usr/local/lib/wlegl_server.so /usr/local/lib/vulkan_x11_stub.so" \
    gnome-shell --wayland --no-x11 \
    2>&1 | tee /tmp/gnome-mali-session.log | systemd-cat -t gnome-mali
SCRIPT
chmod +x /usr/libexec/gnome-mali-session

# -----------------------------------------------------------------------------
# Step 5 — Wayland session desktop entry
# -----------------------------------------------------------------------------
echo "[5/7] Installing wayland session entry..."
backup /usr/share/wayland-sessions/gnome-mali.desktop

cat > /usr/share/wayland-sessions/gnome-mali.desktop << 'EOF'
[Desktop Entry]
Name=GNOME Mali
Comment=GNOME Shell on Mali GPU via HWC2
Exec=/usr/libexec/gnome-mali-session
TryExec=/usr/libexec/gnome-mali-session
Type=Application
DesktopNames=GNOME
X-GDM-SessionRegisters=true
EOF

# -----------------------------------------------------------------------------
# Step 6 — greetd config
# No vendor wrapper needed for phrog — libhybris patch is transparent to phosh
# -----------------------------------------------------------------------------
echo "[6/7] Configuring greetd..."
backup /etc/greetd/phrog.toml

cat > /etc/greetd/phrog.toml << 'EOF'
[terminal]
vt = 7

[default_session]
command = "/usr/libexec/phrog-greetd-session-wrapper"
user = "_greetd"
EOF

# -----------------------------------------------------------------------------
# Step 7 — systemd boot service
# Ensures clean state on boot (no-op now, kept for safety)
# -----------------------------------------------------------------------------
echo "[7/7] Installing systemd boot service..."

cat > /etc/systemd/system/gnome-mali-boot.service << 'EOF'
[Unit]
Description=GNOME Mali boot initialisation
Before=greetd.service
After=local-fs.target

[Service]
Type=oneshot
ExecStart=/bin/true
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable gnome-mali-boot.service

# Record installed state
mkdir -p /var/lib/gnome-mali
date > "$STATE_FILE"

echo ""
echo "=== Installation complete! ==="
echo ""
echo "GNOME Mali is now available in the phrog session menu."
echo "Select 'GNOME Mali' to launch GNOME Shell with Mali GPU acceleration."
echo ""
echo "Pipeline: mutter → libEGL_libhybris.so (patched) → eglplatform_drmadapter.so → HWC2"
echo ""
echo "Session log:  /tmp/gnome-mali-session.log"
echo "Backups:      $BACKUP_DIR"
echo ""
echo "To uninstall, run: sudo ./uninstall-gnome-mali.sh"
