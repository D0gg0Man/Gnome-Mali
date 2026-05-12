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
VENDOR_DIR="/usr/share/glvnd/egl_vendor.d"

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
# Step 1 — Ensure phosh vendor is active before we change anything
# -----------------------------------------------------------------------------
echo "[1/8] Restoring phosh EGL vendor..."
if [ -f /usr/local/bin/gnome-mali-vendor-off ]; then
    /usr/local/bin/gnome-mali-vendor-off 2>/dev/null || true
else
    # Manual restore
    [ -f "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so" ] && \
        cp "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so" /tmp/_phosh_restore.so && \
        mv /tmp/_phosh_restore.so "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"
    rm -f "$VENDOR_DIR/05_hybris_wrapper.json"
    rm -f "$VENDOR_DIR/10_libhybris.json"
    [ -f "$VENDOR_DIR/10_libhybris.json.real" ] && \
        ln -sf "$VENDOR_DIR/10_libhybris.json.real" "$VENDOR_DIR/10_libhybris.json"
    rm -f "$VENDOR_DIR/50_mesa.json"
    [ -f "$VENDOR_DIR/50_mesa.json.real" ] && \
        ln -sf "$VENDOR_DIR/50_mesa.json.real" "$VENDOR_DIR/50_mesa.json"
fi

# -----------------------------------------------------------------------------
# Step 2 — Stop and disable systemd service
# -----------------------------------------------------------------------------
echo "[2/8] Removing systemd service..."
systemctl stop gnome-mali-vendor-restore.service 2>/dev/null || true
systemctl disable gnome-mali-vendor-restore.service 2>/dev/null || true
rm -f /etc/systemd/system/gnome-mali-vendor-restore.service
systemctl daemon-reload

# -----------------------------------------------------------------------------
# Step 3 — Restore greetd config
# -----------------------------------------------------------------------------
echo "[3/8] Restoring greetd config..."
restore_or_remove /etc/greetd/phrog.toml
restore_or_remove /usr/libexec/phrog-greetd-vendor-wrapper

# -----------------------------------------------------------------------------
# Step 4 — Remove session files
# -----------------------------------------------------------------------------
echo "[4/8] Removing session files..."
rm -f /usr/share/wayland-sessions/gnome-mali.desktop
restore_or_remove /usr/libexec/gnome-mali-session

# -----------------------------------------------------------------------------
# Step 5 — Remove vendor swap scripts
# -----------------------------------------------------------------------------
echo "[5/8] Removing vendor swap scripts..."
rm -f /usr/local/bin/gnome-mali-vendor-on
rm -f /usr/local/bin/gnome-mali-vendor-off

# -----------------------------------------------------------------------------
# Step 6 — Restore ld.so.preload
# -----------------------------------------------------------------------------
echo "[6/8] Restoring ld.so.preload..."
BACKUP_PRELOAD="$BACKUP_DIR/$(echo /etc/ld.so.preload | tr '/' '_')"
if [ -f "$BACKUP_PRELOAD" ]; then
    cp -a "$BACKUP_PRELOAD" /etc/ld.so.preload
    echo "  restored: /etc/ld.so.preload"
else
    # Remove only our entries
    sed -i '\|/usr/local/lib/drm_shim.so|d' /etc/ld.so.preload 2>/dev/null || true
    sed -i '\|/usr/local/lib/wlegl_server.so|d' /etc/ld.so.preload 2>/dev/null || true
    echo "  removed gnome-mali entries from /etc/ld.so.preload"
fi

# -----------------------------------------------------------------------------
# Step 7 — Remove installed libraries
# -----------------------------------------------------------------------------
echo "[7/8] Removing installed libraries..."
rm -f /usr/local/lib/drm_shim.so
rm -f /usr/local/lib/wlegl_server.so
rm -f "$EGL_LIB_DIR/libEGL_hybris_wrapper_gnome.so"
rm -f "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so"
rm -f "/usr/lib/aarch64-linux-gnu/libhybris/eglplatform_drmadapter.so"

# Restore original system EGL wrapper if we have a backup
BACKUP_WRAPPER="$BACKUP_DIR/$(echo "$EGL_LIB_DIR/libEGL_hybris_wrapper.so" | tr '/' '_')"
if [ -f "$BACKUP_WRAPPER" ]; then
    cp -a "$BACKUP_WRAPPER" "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"
    echo "  restored: $EGL_LIB_DIR/libEGL_hybris_wrapper.so"
fi

# Remove vendor json files
rm -f /usr/share/gnome-mali/egl_vendor.json
rmdir /usr/share/gnome-mali 2>/dev/null || true

# Restore vendor dir .real files used by our scripts (don't remove the originals)
rm -f "$VENDOR_DIR/05_hybris_wrapper.json"

# Remove sudo rules
echo "[8/8] Removing sudo rules..."
restore_or_remove /etc/sudoers.d/gnome-mali-vendor

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
echo "Backups are preserved at: $BACKUP_DIR"
echo "Remove them manually with: sudo rm -rf $BACKUP_DIR"
