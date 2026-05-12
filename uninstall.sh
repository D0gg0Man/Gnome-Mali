#!/bin/bash
# =============================================================================
# GNOME Mali Session Uninstaller for FuriOS (MT6877 / Dimensity 900)
#
# Usage: sudo ./uninstall-gnome-mali.sh
# =============================================================================

set -e

BACKUP_DIR="/var/lib/gnome-mali/backups"
STATE_FILE="/var/lib/gnome-mali/installed"
EGL_LIB_DIR="/usr/lib/aarch64-linux-gnu"
HYBRIS_PLATFORM_DIR="/usr/lib/aarch64-linux-gnu/libhybris"

echo "=== GNOME Mali Session Uninstaller ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Please run as root (sudo $0)"
    exit 1
fi

if [ ! -f "$STATE_FILE" ]; then
    echo "WARNING: gnome-mali does not appear to be installed (no state file found)."
    echo "Proceeding anyway to clean up any partial installation..."
fi

# -----------------------------------------------------------------------------
# Helper: restore a backed-up file, or remove if no backup exists
# -----------------------------------------------------------------------------
restore_or_remove() {
    local target="$1"
    local backup="$BACKUP_DIR/$(echo "$target" | tr '/' '_')"
    if [ -f "$backup" ]; then
        cp -a "$backup" "$target"
        echo "  restored: $target"
    elif [ -e "$target" ]; then
        rm -f "$target"
        echo "  removed:  $target"
    fi
}

# -----------------------------------------------------------------------------
# Step 1 — Restore patched libhybris EGL to original
# -----------------------------------------------------------------------------
echo "[1/6] Restoring libhybris EGL..."
restore_or_remove "$EGL_LIB_DIR/libEGL_libhybris.so.0.0.0"
ldconfig

# -----------------------------------------------------------------------------
# Step 2 — Stop and disable systemd service
# -----------------------------------------------------------------------------
echo "[2/6] Removing systemd service..."
systemctl stop gnome-mali-boot.service 2>/dev/null || true
systemctl disable gnome-mali-boot.service 2>/dev/null || true
rm -f /etc/systemd/system/gnome-mali-boot.service
# Also clean up old vendor-restore service if present
systemctl stop gnome-mali-vendor-restore.service 2>/dev/null || true
systemctl disable gnome-mali-vendor-restore.service 2>/dev/null || true
rm -f /etc/systemd/system/gnome-mali-vendor-restore.service
systemctl daemon-reload

# -----------------------------------------------------------------------------
# Step 3 — Restore greetd config
# -----------------------------------------------------------------------------
echo "[3/6] Restoring greetd config..."
restore_or_remove /etc/greetd/phrog.toml
# Remove vendor wrapper if present from old install
rm -f /usr/libexec/phrog-greetd-vendor-wrapper

# -----------------------------------------------------------------------------
# Step 4 — Remove session files
# -----------------------------------------------------------------------------
echo "[4/6] Removing session files..."
rm -f /usr/share/wayland-sessions/gnome-mali.desktop
restore_or_remove /usr/libexec/gnome-mali-session

# -----------------------------------------------------------------------------
# Step 5 — Remove installed libraries
# -----------------------------------------------------------------------------
echo "[5/6] Removing installed libraries..."

# Remove drmadapter platform
rm -f "$HYBRIS_PLATFORM_DIR/eglplatform_drmadapter.so"

# Remove shim libraries
rm -f /usr/local/lib/drm_shim.so
rm -f /usr/local/lib/wlegl_server.so
rm -f /usr/local/lib/vulkan_x11_stub.so

# Remove old vendor wrapper libraries if present
rm -f "$EGL_LIB_DIR/libEGL_hybris_wrapper_gnome.so"
rm -f "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so"

# Remove old vendor swap scripts if present
rm -f /usr/local/bin/gnome-mali-vendor-on
rm -f /usr/local/bin/gnome-mali-vendor-off

# Remove old sudo rules if present
rm -f /etc/sudoers.d/gnome-mali-vendor

# Remove old vendor json files if present
rm -f /usr/share/gnome-mali/egl_vendor.json
rmdir /usr/share/gnome-mali 2>/dev/null || true

# -----------------------------------------------------------------------------
# Step 6 — Restore ld.so.preload
# -----------------------------------------------------------------------------
echo "[6/6] Restoring ld.so.preload..."
BACKUP_PRELOAD="$BACKUP_DIR/$(echo /etc/ld.so.preload | tr '/' '_')"
if [ -f "$BACKUP_PRELOAD" ]; then
    cp -a "$BACKUP_PRELOAD" /etc/ld.so.preload
    echo "  restored: /etc/ld.so.preload"
else
    sed -i '\|/usr/local/lib/drm_shim.so|d'        /etc/ld.so.preload 2>/dev/null || true
    sed -i '\|/usr/local/lib/wlegl_server.so|d'   /etc/ld.so.preload 2>/dev/null || true
    sed -i '\|/usr/local/lib/vulkan_x11_stub.so|d' /etc/ld.so.preload 2>/dev/null || true
    echo "  removed gnome-mali entries from /etc/ld.so.preload"
fi

# -----------------------------------------------------------------------------
# Clean up state
# -----------------------------------------------------------------------------
rm -f "$STATE_FILE"
rmdir /var/lib/gnome-mali 2>/dev/null || true

# Restart greetd to apply changes
echo ""
echo "Restarting greetd..."
systemctl restart greetd

echo ""
echo "=== Uninstallation complete! ==="
echo ""
echo "GNOME Mali has been removed. Phosh is now the only session."
echo "Backups preserved at: $BACKUP_DIR"
echo "Remove them manually with: sudo rm -rf $BACKUP_DIR"
