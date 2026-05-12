#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/gralloc/gralloc.h>

typedef EGLBoolean (*PFNEGLHYBRISCREATENATIVEBUFFERPROC)(EGLint w, EGLint h, EGLint usage, EGLint format, EGLint *stride, EGLClientBuffer *buf);
typedef EGLBoolean (*PFNEGLHYBRISSERIALIZENATIVEBUFFERPROC)(EGLClientBuffer buf, int *ints, int *fds);
typedef EGLBoolean (*PFNEGLHYBRISGETNATIVEBUFFERINFOPROC)(EGLClientBuffer buf, int *num_ints, int *num_fds);
typedef EGLBoolean (*PFNEGLHYBRISRELEASENATIVEBUFFERPROC)(EGLClientBuffer buf);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);

static PFNEGLHYBRISCREATENATIVEBUFFERPROC eglHybrisCreateNativeBuffer;
static PFNEGLHYBRISSERIALIZENATIVEBUFFERPROC eglHybrisSerializeNativeBuffer;
static PFNEGLHYBRISGETNATIVEBUFFERINFOPROC eglHybrisGetNativeBufferInfo;
static PFNEGLHYBRISRELEASENATIVEBUFFERPROC eglHybrisReleaseNativeBuffer;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
static PFNEGLCREATEIMAGEKHRPROC _eglCreateImageKHR;

static hwc2_compat_device_t *hwc2_dev = NULL;
static hwc2_compat_display_t *hwc2_disp = NULL;
static hwc2_compat_layer_t *hwc2_layer = NULL;

static void on_hotplug(HWC2EventListener *s, int32_t seq, hwc2_display_t d, bool c, bool p) {}
static void on_vsync(HWC2EventListener *s, int32_t seq, hwc2_display_t d, int64_t ts) {}
static void on_refresh(HWC2EventListener *s, int32_t seq, hwc2_display_t d) {}

static void present_cb(void *ud, struct ANativeWindow *win, struct ANativeWindowBuffer *buf) {
    uint32_t nt=0, nr=0;
    hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    if (nt) hwc2_compat_display_accept_changes(hwc2_disp);
    hwc2_compat_layer_set_buffer(hwc2_layer, 0, buf, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, 0, buf, -1, 0);
    int32_t fence=-1;
    hwc2_compat_display_present(hwc2_disp, &fence);
    if (fence>=0) close(fence);
    HWCNativeBufferSetFence(buf, -1);
}

int main() {
    setenv("EGL_PLATFORM", "hwcomposer", 1);
    setenv("HYBRIS_EGLPLATFORM", "hwcomposer", 1);

    /* Init gralloc first */
    printf("Init gralloc...\n");
    hybris_gralloc_initialize(0);
    printf("gralloc ok\n");

    hwc2_dev = hwc2_compat_device_new(false);
    if (!hwc2_dev) { fprintf(stderr, "no hwc2\n"); return 1; }
    printf("HWC2 ok\n");

    HWC2EventListener listener = { on_vsync, on_hotplug, on_refresh };
    hwc2_compat_device_register_callback(hwc2_dev, &listener, 0);
    hwc2_compat_device_on_hotplug(hwc2_dev, 0, true);
    hwc2_disp = hwc2_compat_device_get_display_by_id(hwc2_dev, 0);
    hwc2_compat_display_set_power_mode(hwc2_disp, 2);
    hwc2_layer = hwc2_compat_display_create_layer(hwc2_disp);
    hwc2_compat_layer_set_composition_type(hwc2_layer, 4);
    hwc2_compat_layer_set_blend_mode(hwc2_layer, 1);
    hwc2_compat_layer_set_source_crop(hwc2_layer, 0, 0, 1080, 2412);
    hwc2_compat_layer_set_display_frame(hwc2_layer, 0, 0, 1080, 2412);
    hwc2_compat_layer_set_visible_region(hwc2_layer, 0, 0, 1080, 2412);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    eglInitialize(dpy, &maj, &min);
    printf("EGL %d.%d\n", maj, min);
    eglBindAPI(EGL_OPENGL_ES_API);

    eglHybrisCreateNativeBuffer = (PFNEGLHYBRISCREATENATIVEBUFFERPROC)
        eglGetProcAddress("eglHybrisCreateNativeBuffer");
    eglHybrisSerializeNativeBuffer = (PFNEGLHYBRISSERIALIZENATIVEBUFFERPROC)
        eglGetProcAddress("eglHybrisSerializeNativeBuffer");
    eglHybrisGetNativeBufferInfo = (PFNEGLHYBRISGETNATIVEBUFFERINFOPROC)
        eglGetProcAddress("eglHybrisGetNativeBufferInfo");
    _eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");

    /* Try different usage combinations */
    EGLClientBuffer native_buf = NULL;
    EGLint stride = 0;
    int usages[] = {0xB00, 0x900, 0x300, 0x33, 0xB33, 0x933, 0};
    for (int i = 0; usages[i]; i++) {
        EGLBoolean ok = eglHybrisCreateNativeBuffer(1080, 2412, usages[i], 1, &stride, &native_buf);
        printf("usage=0x%x: ok=%d buf=%p stride=%d\n", usages[i], ok, (void*)native_buf, stride);
        if (ok && native_buf) break;
        native_buf = NULL;
    }

    if (!native_buf) { fprintf(stderr, "all usages failed\n"); return 1; }

    int num_ints=0, num_fds=0;
    eglHybrisGetNativeBufferInfo(native_buf, &num_ints, &num_fds);
    printf("buffer: %d ints %d fds stride=%d\n", num_ints, num_fds, stride);

    int *ints = calloc(num_ints+1, sizeof(int));
    int *fds = calloc(num_fds+1, sizeof(int));
    eglHybrisSerializeNativeBuffer(native_buf, ints, fds);
    for (int i=0; i<num_fds; i++) printf("fd[%d]=%d\n", i, fds[i]);

    printf("SUCCESS - native buffer works!\n");
    free(ints); free(fds);
    return 0;
}
