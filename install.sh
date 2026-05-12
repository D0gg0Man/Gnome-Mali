#!/bin/bash
# =============================================================================
# GNOME Mali Session Installer for FuriOS (MT6877 / Dimensity 900)
# Installs GNOME Shell alongside Phosh via the phrog greeter
#
# Usage: sudo ./install-gnome-mali.sh
#
# Required files (same directory as this script):
#   libEGL_hybris_wrapper_gnome.so
#   libEGL_hybris_wrapper_phosh.so
#   eglplatform_drmadapter.so
#   drm_shim.so
#   wlegl_server.so
# =============================================================================

set -e

INSTALL_DIR="$(dirname "$(readlink -f "$0")")"
EGL_LIB_DIR="/usr/lib/aarch64-linux-gnu"
HYBRIS_PLATFORM_DIR="/usr/lib/aarch64-linux-gnu/libhybris"
VENDOR_DIR="/usr/share/glvnd/egl_vendor.d"
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
    libEGL_hybris_wrapper_gnome.so
    libEGL_hybris_wrapper_phosh.so
    eglplatform_drmadapter.so
    drm_shim.so
    wlegl_server.so
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
# Step 1 — EGL wrapper libraries
# -----------------------------------------------------------------------------
echo "[1/9] Installing EGL wrapper libraries..."
backup "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"

cp "$INSTALL_DIR/libEGL_hybris_wrapper_gnome.so"  /tmp/_gnome_wrapper.so
mv /tmp/_gnome_wrapper.so "$EGL_LIB_DIR/libEGL_hybris_wrapper_gnome.so"
chown root:root "$EGL_LIB_DIR/libEGL_hybris_wrapper_gnome.so"
chmod 755 "$EGL_LIB_DIR/libEGL_hybris_wrapper_gnome.so"

cp "$INSTALL_DIR/libEGL_hybris_wrapper_phosh.so"  /tmp/_phosh_wrapper.so
mv /tmp/_phosh_wrapper.so "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so"
chown root:root "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so"
chmod 755 "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so"

# Ensure phosh wrapper is the active system wrapper (safe default)
cp "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so" /tmp/_active_wrapper.so
mv /tmp/_active_wrapper.so "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"
chown root:root "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"
chmod 755 "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"

# -----------------------------------------------------------------------------
# Step 2 — Hybris EGL platform + shim libraries
# -----------------------------------------------------------------------------
echo "[2/9] Installing hybris platform and shim libraries..."
backup /usr/local/lib/drm_shim.so
backup /usr/local/lib/wlegl_server.so

# drmadapter hybris EGL platform — handles HWC2 init and present
mkdir -p "$HYBRIS_PLATFORM_DIR"
cp "$INSTALL_DIR/eglplatform_drmadapter.so" /tmp/_drmadapter.so
mv /tmp/_drmadapter.so "$HYBRIS_PLATFORM_DIR/eglplatform_drmadapter.so"
chown root:root "$HYBRIS_PLATFORM_DIR/eglplatform_drmadapter.so"
chmod 755 "$HYBRIS_PLATFORM_DIR/eglplatform_drmadapter.so"

cp "$INSTALL_DIR/drm_shim.so"    /tmp/_drm_shim.so
mv /tmp/_drm_shim.so    /usr/local/lib/drm_shim.so
cp "$INSTALL_DIR/wlegl_server.so" /tmp/_wlegl.so
mv /tmp/_wlegl.so       /usr/local/lib/wlegl_server.so

# -----------------------------------------------------------------------------
# Step 3 — /etc/ld.so.preload
# -----------------------------------------------------------------------------
echo "[3/9] Configuring ld.so.preload..."
backup /etc/ld.so.preload

touch /etc/ld.so.preload
grep -qxF '/usr/local/lib/drm_shim.so'    /etc/ld.so.preload || \
    echo '/usr/local/lib/drm_shim.so'    >> /etc/ld.so.preload
grep -qxF '/usr/local/lib/wlegl_server.so' /etc/ld.so.preload || \
    echo '/usr/local/lib/wlegl_server.so' >> /etc/ld.so.preload

# -----------------------------------------------------------------------------
# Step 4 — GLVND vendor json files
# -----------------------------------------------------------------------------
echo "[4/9] Configuring EGL vendor files..."
mkdir -p /usr/share/gnome-mali

backup "$VENDOR_DIR/10_libhybris.json"
backup "$VENDOR_DIR/50_mesa.json"

if [ ! -f "$VENDOR_DIR/10_libhybris.json.real" ]; then
    [ -f "$VENDOR_DIR/10_libhybris.json" ] && \
        cp "$VENDOR_DIR/10_libhybris.json" "$VENDOR_DIR/10_libhybris.json.real"
fi
if [ ! -f "$VENDOR_DIR/50_mesa.json.real" ]; then
    [ -f "$VENDOR_DIR/50_mesa.json" ] && \
        cp "$VENDOR_DIR/50_mesa.json" "$VENDOR_DIR/50_mesa.json.real"
fi

# Restore vendor dir to clean phosh state
rm -f "$VENDOR_DIR/05_hybris_wrapper.json"
rm -f "$VENDOR_DIR/10_libhybris.json"
[ -f "$VENDOR_DIR/10_libhybris.json.real" ] && \
    ln -sf "$VENDOR_DIR/10_libhybris.json.real" "$VENDOR_DIR/10_libhybris.json"
rm -f "$VENDOR_DIR/50_mesa.json"
[ -f "$VENDOR_DIR/50_mesa.json.real" ] && \
    ln -sf "$VENDOR_DIR/50_mesa.json.real" "$VENDOR_DIR/50_mesa.json"

cat > /usr/share/gnome-mali/egl_vendor.json << 'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "libEGL_hybris_wrapper.so"
    }
}
JSON

# -----------------------------------------------------------------------------
# Step 5 — Vendor swap scripts
# -----------------------------------------------------------------------------
echo "[5/9] Installing vendor swap scripts..."

cat > /usr/local/bin/gnome-mali-vendor-on << 'SCRIPT'
#!/bin/bash
# Swap EGL vendor to GNOME Mali wrapper (called at session start)
EGL_LIB_DIR="/usr/lib/aarch64-linux-gnu"
VENDOR_DIR="/usr/share/glvnd/egl_vendor.d"
LOG="/tmp/vendor-on.log"

echo "$(date): vendor-on starting" >> "$LOG"

cp "$EGL_LIB_DIR/libEGL_hybris_wrapper_gnome.so" /tmp/_gnome_vendor.so \
    && echo "cp ok" >> "$LOG" \
    || { echo "cp FAILED" >> "$LOG"; exit 1; }
mv /tmp/_gnome_vendor.so "$EGL_LIB_DIR/libEGL_hybris_wrapper.so" \
    && echo "mv ok" >> "$LOG" \
    || { echo "mv FAILED" >> "$LOG"; exit 1; }
chown root:root "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"
chmod 755 "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"

# Disable libhybris vendor (conflicts with our wrapper)
rm -f "$VENDOR_DIR/10_libhybris.json"
ln -sf /dev/null "$VENDOR_DIR/10_libhybris.json" \
    && echo "ln ok" >> "$LOG" \
    || echo "ln FAILED" >> "$LOG"

# Keep Mesa vendor active — GLVND needs 2+ vendors to iterate the list
rm -f "$VENDOR_DIR/50_mesa.json"
ln -sf "$VENDOR_DIR/50_mesa.json.real" "$VENDOR_DIR/50_mesa.json" \
    && echo "mesa ok" >> "$LOG" \
    || echo "mesa FAILED" >> "$LOG"

cat > "$VENDOR_DIR/05_hybris_wrapper.json" << 'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "libEGL_hybris_wrapper.so"
    }
}
JSON
echo "json ok" >> "$LOG"
echo "$(date): vendor-on done" >> "$LOG"
SCRIPT
chmod +x /usr/local/bin/gnome-mali-vendor-on

cat > /usr/local/bin/gnome-mali-vendor-off << 'SCRIPT'
#!/bin/bash
# Restore EGL vendor to Phosh wrapper (called at session end / boot)
EGL_LIB_DIR="/usr/lib/aarch64-linux-gnu"
VENDOR_DIR="/usr/share/glvnd/egl_vendor.d"

cp "$EGL_LIB_DIR/libEGL_hybris_wrapper_phosh.so" /tmp/_phosh_vendor.so
mv /tmp/_phosh_vendor.so "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"
chown root:root "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"
chmod 755 "$EGL_LIB_DIR/libEGL_hybris_wrapper.so"

rm -f "$VENDOR_DIR/05_hybris_wrapper.json"
rm -f "$VENDOR_DIR/10_libhybris.json"
[ -f "$VENDOR_DIR/10_libhybris.json.real" ] && \
    ln -sf "$VENDOR_DIR/10_libhybris.json.real" "$VENDOR_DIR/10_libhybris.json"
rm -f "$VENDOR_DIR/50_mesa.json"
[ -f "$VENDOR_DIR/50_mesa.json.real" ] && \
    ln -sf "$VENDOR_DIR/50_mesa.json.real" "$VENDOR_DIR/50_mesa.json"
SCRIPT
chmod +x /usr/local/bin/gnome-mali-vendor-off

# -----------------------------------------------------------------------------
# Step 6 — Session wrapper
# -----------------------------------------------------------------------------
echo "[6/9] Installing session wrapper..."
backup /usr/libexec/gnome-mali-session

cat > /usr/libexec/gnome-mali-session << 'SCRIPT'
#!/bin/bash
# GNOME Mali session launcher — called by greetd via gnome-mali.desktop

export GBM_BACKEND=hybris
export GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm
export GSK_RENDERER=gl
export GDK_BACKEND=wayland
export GDK_GL=gles
export XDG_CURRENT_DESKTOP=GNOME
export XDG_SESSION_DESKTOP=gnome
export MUTTER_DEBUG_FORCE_KMS_MODE=simple
export HYBRIS_EGLPLATFORM=drmadapter

# Clear variables inherited from phrog that confuse mutter/GLVND
unset WLR_BACKENDS WLR_HWC_SKIP_VERSION_CHECK EGL_PLATFORM

# Install GNOME Mali EGL vendor
sudo /usr/local/bin/gnome-mali-vendor-on

# Start gsd-media-keys once wayland socket is ready
(
    for i in $(seq 1 20); do
        [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break
        sleep 0.5
    done
    export WAYLAND_DISPLAY=wayland-0
    /usr/libexec/gsd-media-keys 2>/dev/null &
) &

# Launch gnome-shell — clear __EGL_VENDOR_LIBRARY_FILENAMES so GLVND
# scans the vendor directory (needs 2+ vendors to call getPlatformDisplay)
exec env -u __EGL_VENDOR_LIBRARY_FILENAMES \
    LD_PRELOAD="/usr/local/lib/drm_shim.so /usr/local/lib/wlegl_server.so" \
    gnome-shell --wayland --no-x11 \
    2>&1 | tee /tmp/gnome-mali-session.log | systemd-cat -t gnome-mali
SCRIPT
chmod +x /usr/libexec/gnome-mali-session

# -----------------------------------------------------------------------------
# Step 7 — Wayland session desktop entry
# -----------------------------------------------------------------------------
echo "[7/9] Installing wayland session entry..."
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
# Step 8 — phrog greeter wrapper + greetd config
# -----------------------------------------------------------------------------
echo "[8/9] Configuring phrog greeter..."
backup /usr/libexec/phrog-greetd-vendor-wrapper
backup /etc/greetd/phrog.toml

cat > /usr/libexec/phrog-greetd-vendor-wrapper << 'SCRIPT'
#!/bin/bash
# Restore phosh EGL vendor before launching phrog (in case gnome-mali left it set)
sudo /usr/local/bin/gnome-mali-vendor-off 2>/dev/null || true
exec /usr/libexec/phrog-greetd-session-wrapper "$@"
SCRIPT
chmod +x /usr/libexec/phrog-greetd-vendor-wrapper

cat > /etc/greetd/phrog.toml << 'EOF'
[terminal]
vt = 7

[default_session]
command = "/usr/libexec/phrog-greetd-vendor-wrapper"
user = "_greetd"
EOF

# -----------------------------------------------------------------------------
# Step 9 — sudo rules + systemd service
# -----------------------------------------------------------------------------
echo "[9/9] Installing sudo rules and systemd service..."
backup /etc/sudoers.d/gnome-mali-vendor

cat > /etc/sudoers.d/gnome-mali-vendor << 'EOF'
# Allow gnome-mali session to swap EGL vendors without password prompt
furios  ALL=(root) NOPASSWD: /usr/local/bin/gnome-mali-vendor-on, /usr/local/bin/gnome-mali-vendor-off
_greetd ALL=(root) NOPASSWD: /usr/local/bin/gnome-mali-vendor-off
EOF
chmod 440 /etc/sudoers.d/gnome-mali-vendor

cat > /etc/systemd/system/gnome-mali-vendor-restore.service << 'EOF'
[Unit]
Description=Restore phosh EGL vendor after gnome-mali session
Before=greetd.service
After=local-fs.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/gnome-mali-vendor-off
ExecStop=/usr/local/bin/gnome-mali-vendor-off
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable gnome-mali-vendor-restore.service

# Ensure phosh vendor is active right now
/usr/local/bin/gnome-mali-vendor-off

# Record installed state
mkdir -p /var/lib/gnome-mali
date > "$STATE_FILE"

echo ""
echo "=== Installation complete! ==="
echo ""
echo "GNOME Mali is now available in the phrog session menu."
echo "Select 'GNOME Mali' to launch GNOME Shell with Mali GPU acceleration."
echo ""
echo "Session log:  /tmp/gnome-mali-session.log"
echo "Vendor log:   /tmp/vendor-on.log"
echo "Backups:      $BACKUP_DIR"
echo ""
echo "To uninstall, run: sudo ./uninstall-gnome-mali.sh"
