/*
 * egl_platform_shim.c
 * Intercepts eglGetPlatformDisplay before libhybris sees it,
 * remaps EGL_PLATFORM_GBM_KHR to drmadapter platform.
 * Load via LD_PRELOAD before libEGL_libhybris.so
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define LOG(fmt, ...) fprintf(stderr, "egl_shim: " fmt "\n", ##__VA_ARGS__)

/* Forward to libhybris's internal platform display function */
typedef EGLDisplay (*get_platform_fn)(EGLenum, void*, const EGLAttrib*);

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *display_id,
                                  const EGLAttrib *attrib_list) {
    LOG("eglGetPlatformDisplay platform=0x%x", platform);

    /* Remap GBM to EGL_NONE so libhybris uses HYBRIS_EGLPLATFORM */
    if (platform == 0x31D7 /* EGL_PLATFORM_GBM_KHR */) {
        LOG("remapping GBM -> EGL_NONE (drmadapter)");
        platform = EGL_NONE;
    }

    get_platform_fn real = (get_platform_fn)dlsym(RTLD_NEXT, "eglGetPlatformDisplay");
    if (!real) {
        LOG("no real eglGetPlatformDisplay found");
        return EGL_NO_DISPLAY;
    }
    return real(platform, display_id, attrib_list);
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *display_id,
                                     const EGLint *attrib_list) {
    LOG("eglGetPlatformDisplayEXT platform=0x%x", platform);
    return eglGetPlatformDisplay(platform, display_id, (const EGLAttrib*)attrib_list);
}
