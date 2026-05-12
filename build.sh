#!/bin/bash
set -e
cd ~/gnome-mali

echo "Building DRM shim..."
sudo gcc -shared -fPIC -O2 -o /usr/local/lib/drm_shim.so src/drm_ioctl_shim.c \
    -ldl -ldrm -lgralloc -lrt -I/usr/include/libdrm -I/usr/include -I/usr/include/android \
    -Wl,-soname,drm_shim.so

echo "Building EGL vendor wrapper..."
sudo gcc -shared -fPIC -O2 -o /usr/lib/aarch64-linux-gnu/libEGL_hybris_wrapper.so src/egl_hybris_vendor_v2.c \
    -I/usr/include -ldl -lrt -Wl,-soname,libEGL_hybris_wrapper.so

echo "Building GBM backend..."
sudo gcc -shared -fPIC -O2 -o /usr/lib/aarch64-linux-gnu/gbm/hybris_gbm.so src/gbm_hybris.c \
    -I/usr/include -I/usr/include/android -ldl -lgralloc -Wl,-soname,hybris_gbm.so

echo "Building wlegl server..."
sudo gcc -shared -fPIC -O2 -o /usr/local/lib/wlegl_server.so src/wlegl_server.c \
    -ldl -Wl,-soname,wlegl_server.so

sudo cp /usr/local/lib/drm_shim.so built/
sudo cp /usr/lib/aarch64-linux-gnu/libEGL_hybris_wrapper.so built/
sudo cp /usr/lib/aarch64-linux-gnu/gbm/hybris_gbm.so built/
sudo cp /usr/local/lib/wlegl_server.so built/
echo "All built and installed!"
