#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static hwc2_compat_device_t *hwc2_dev = NULL;
static hwc2_compat_display_t *hwc2_disp = NULL;
static hwc2_compat_layer_t *hwc2_layer = NULL;

static void on_hotplug(HWC2EventListener *self, int32_t seq,
                       hwc2_display_t display, bool connected, bool primary) {
    printf("hotplug: display=%llu connected=%d primary=%d\n",
           (unsigned long long)display, connected, primary);
}
static void on_vsync(HWC2EventListener *self, int32_t seq,
                     hwc2_display_t display, int64_t ts) {}
static void on_refresh(HWC2EventListener *self, int32_t seq,
                       hwc2_display_t display) {}

static void present_cb(void *user_data, struct ANativeWindow *win,
                       struct ANativeWindowBuffer *buf) {
    printf("present_cb buf=%p\n", buf);
    uint32_t n_types=0, n_reqs=0;
    hwc2_compat_display_validate(hwc2_disp, &n_types, &n_reqs);
    if (n_types) hwc2_compat_display_accept_changes(hwc2_disp);
    hwc2_compat_layer_set_buffer(hwc2_layer, 0, buf, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, 0, buf, -1, 0);
    int32_t fence = -1;
    hwc2_compat_display_present(hwc2_disp, &fence);
    printf("presented fence=%d\n", fence);
    if (fence >= 0) close(fence);
    HWCNativeBufferSetFence(buf, -1);
}

int main() {
    setenv("EGL_PLATFORM", "hwcomposer", 1);
    setenv("HYBRIS_EGLPLATFORM", "hwcomposer", 1);

    printf("hwc2_compat_device_new...\n"); fflush(stdout);
    hwc2_dev = hwc2_compat_device_new(false);
    if (!hwc2_dev) { fprintf(stderr, "no device\n"); return 1; }
    printf("device ok\n"); fflush(stdout);

    HWC2EventListener listener = {
        .on_vsync_received = on_vsync,
        .on_hotplug_received = on_hotplug,
        .on_refresh_received = on_refresh,
    };
    hwc2_compat_device_register_callback(hwc2_dev, &listener, 0);
    printf("callback registered\n"); fflush(stdout);

    hwc2_compat_device_on_hotplug(hwc2_dev, 0, true);
    printf("hotplug triggered\n"); fflush(stdout);

    hwc2_disp = hwc2_compat_device_get_display_by_id(hwc2_dev, 0);
    if (!hwc2_disp) { fprintf(stderr, "no display\n"); return 1; }
    printf("display ok\n"); fflush(stdout);

    HWC2DisplayConfig *cfg = hwc2_compat_display_get_active_config(hwc2_disp);
    if (cfg) { printf("config: %dx%d\n", cfg->width, cfg->height); free(cfg); }

    hwc2_compat_display_set_power_mode(hwc2_disp, 2);
    printf("power on\n"); fflush(stdout);

    hwc2_layer = hwc2_compat_display_create_layer(hwc2_disp);
    printf("layer: %p\n", hwc2_layer); fflush(stdout);

    hwc2_compat_layer_set_composition_type(hwc2_layer, 4);
    hwc2_compat_layer_set_blend_mode(hwc2_layer, 1);
    hwc2_compat_layer_set_source_crop(hwc2_layer, 0, 0, 1080, 2412);
    hwc2_compat_layer_set_display_frame(hwc2_layer, 0, 0, 1080, 2412);
    hwc2_compat_layer_set_visible_region(hwc2_layer, 0, 0, 1080, 2412);
    printf("layer configured\n"); fflush(stdout);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    eglInitialize(dpy, &maj, &min);
    printf("EGL %d.%d\n", maj, min); fflush(stdout);

    struct ANativeWindow *win = HWCNativeWindowCreate(1080, 2412, 1, present_cb, NULL);
    printf("window: %p\n", win); fflush(stdout);

    EGLint attrs[] = {
        EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8,
        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_NONE
    };
    EGLConfig cfg2; EGLint n;
    eglChooseConfig(dpy, attrs, &cfg2, 1, &n);
    printf("configs: %d\n", n); fflush(stdout);

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg2, (EGLNativeWindowType)win, NULL);
    printf("surface: %p err=0x%x\n", surf, eglGetError()); fflush(stdout);

    EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg2, EGL_NO_CONTEXT, ctx_attrs);
    printf("context: %p\n", ctx); fflush(stdout);

    eglMakeCurrent(dpy, surf, surf, ctx);
    printf("made current\n"); fflush(stdout);

    glClearColor(1,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    printf("swapping...\n"); fflush(stdout);
    eglSwapBuffers(dpy, surf);
    printf("swapped!\n"); fflush(stdout);

    sleep(3);
    return 0;
}
