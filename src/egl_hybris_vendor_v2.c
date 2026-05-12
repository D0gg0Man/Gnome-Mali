#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <glvnd/libeglabi.h>

#define LOG(fmt, ...) fprintf(stderr, "hybris_vendor: " fmt "\n", ##__VA_ARGS__)
#define FRAME_W 1080
#define FRAME_H 2412
#define FRAME_SIZE (FRAME_W * FRAME_H * 4)
#define SHM_NAME "/gnome_mali_frame"

static void *frame_shm = NULL;
static EGLDisplay our_display = EGL_NO_DISPLAY;

static void ensure_shm(void) {
    if (frame_shm) return;
    int fd = shm_open(SHM_NAME, O_CREAT|O_RDWR, 0666);
    if (fd < 0) return;
    ftruncate(fd, FRAME_SIZE);
    frame_shm = mmap(NULL, FRAME_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (frame_shm == MAP_FAILED) frame_shm = NULL;
}

static void *hybris_lib = NULL;
static __EGLapiImports hybris_imports;
static EGLBoolean (*hybris_eglGetConfigAttrib)(EGLDisplay,EGLConfig,EGLint,EGLint*) = NULL;
static EGLSurface (*hybris_eglCreateWindowSurface)(EGLDisplay,EGLConfig,EGLNativeWindowType,const EGLint*) = NULL;
static EGLSurface (*hybris_eglCreatePbufferSurface)(EGLDisplay,EGLConfig,const EGLint*) = NULL;
static EGLBoolean (*hybris_eglSwapBuffers)(EGLDisplay,EGLSurface) = NULL;

static uint32_t android_to_gbm_format(uint32_t fmt, EGLint alpha) {
    switch (fmt) {
        case 1: return alpha > 0 ? 0x34324241 : 0x34324258;
        case 2: return 0x34324258;
        case 5: return alpha > 0 ? 0x34325241 : 0x34325258;
        case 4: return 0x36314752;
        default: return 0x34324258;
    }
}

static EGLBoolean my_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                         EGLint attribute, EGLint *value) {
    EGLBoolean ret = hybris_eglGetConfigAttrib(dpy, config, attribute, value);
    if (ret && attribute == EGL_NATIVE_VISUAL_ID) {
        EGLint alpha = 0;
        hybris_eglGetConfigAttrib(dpy, config, EGL_ALPHA_SIZE, &alpha);
        *value = (EGLint)android_to_gbm_format((uint32_t)*value, alpha);
    }
    return ret;
}

static EGLSurface my_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                              EGLNativeWindowType win,
                                              const EGLint *attrib_list) {
    /* Only intercept for our GBM/Android display - let Mesa handle wayland displays */
    if (dpy == our_display) {
        LOG("eglCreateWindowSurface GBM display -> pbuffer %dx%d", FRAME_W, FRAME_H);
        EGLint pbuf_attribs[] = { EGL_WIDTH, FRAME_W, EGL_HEIGHT, FRAME_H, EGL_NONE };
        EGLSurface surf = hybris_eglCreatePbufferSurface(dpy, config, pbuf_attribs);
        LOG("pbuffer: %p", (void*)surf);
        return surf;
    }
    /* Wayland client surface - pass through to hybris wayland platform */
    LOG("eglCreateWindowSurface wayland client dpy=%p -> passthrough", (void*)dpy);
    return hybris_eglCreateWindowSurface(dpy, config, win, attrib_list);
}

static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    EGLBoolean ret = hybris_eglSwapBuffers ? hybris_eglSwapBuffers(dpy, surface) : EGL_TRUE;
    if (dpy == our_display) {
        ensure_shm();
        if (frame_shm)
            glReadPixels(0, 0, FRAME_W, FRAME_H, GL_RGBA, GL_UNSIGNED_BYTE, frame_shm);
    }
    return ret;
}

static EGLDisplay wrapped_getPlatformDisplay(EGLenum platform, void *nativeDisplay,
                                              const EGLAttrib *attrib_list) {
    LOG("getPlatformDisplay platform=0x%x", platform);
    if (platform != EGL_PLATFORM_GBM_KHR && platform != EGL_NONE) {
        LOG("Not GBM platform, declining");
        return EGL_NO_DISPLAY;
    }
    EGLDisplay dpy = hybris_imports.getPlatformDisplay(EGL_NONE, EGL_DEFAULT_DISPLAY, NULL);
    our_display = dpy;
    LOG("our_display = %p", (void*)dpy);
    return dpy;
}

static void *wrapped_getProcAddress(const char *procName) {
    if (strcmp(procName, "eglGetConfigAttrib") == 0) return (void*)my_eglGetConfigAttrib;
    if (strcmp(procName, "eglCreateWindowSurface") == 0) return (void*)my_eglCreateWindowSurface;
    if (strcmp(procName, "eglSwapBuffers") == 0) return (void*)my_eglSwapBuffers;
    return hybris_imports.getProcAddress ? hybris_imports.getProcAddress(procName) : NULL;
}

static void *wrapped_getDispatchAddress(const char *procName) {
    if (strcmp(procName, "eglGetConfigAttrib") == 0) return (void*)my_eglGetConfigAttrib;
    if (strcmp(procName, "eglCreateWindowSurface") == 0) return (void*)my_eglCreateWindowSurface;
    if (strcmp(procName, "eglSwapBuffers") == 0) return (void*)my_eglSwapBuffers;
    return hybris_imports.getDispatchAddress ? hybris_imports.getDispatchAddress(procName) : NULL;
}

EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports *exports,
                       __EGLvendorInfo *vendor, __EGLapiImports *imports) {
    LOG("__egl_Main version=0x%x", version);
    hybris_lib = dlopen("libEGL_libhybris.so.0", RTLD_NOW|RTLD_GLOBAL);
    if (!hybris_lib) { LOG("dlopen failed: %s", dlerror()); return EGL_FALSE; }
    typedef EGLBoolean (*egl_main_t)(uint32_t,const __EGLapiExports*,__EGLvendorInfo*,__EGLapiImports*);
    egl_main_t hybris_egl_main = dlsym(hybris_lib, "__egl_Main");
    if (!hybris_egl_main) { LOG("no __egl_Main"); return EGL_FALSE; }
    memset(&hybris_imports, 0, sizeof(hybris_imports));
    if (!hybris_egl_main(version, exports, vendor, &hybris_imports)) {
        LOG("hybris __egl_Main failed"); return EGL_FALSE;
    }
    if (hybris_imports.getProcAddress) {
        hybris_eglGetConfigAttrib = hybris_imports.getProcAddress("eglGetConfigAttrib");
        hybris_eglCreateWindowSurface = hybris_imports.getProcAddress("eglCreateWindowSurface");
        hybris_eglCreatePbufferSurface = hybris_imports.getProcAddress("eglCreatePbufferSurface");
        hybris_eglSwapBuffers = hybris_imports.getProcAddress("eglSwapBuffers");
    }
    *imports = hybris_imports;
    imports->getPlatformDisplay = wrapped_getPlatformDisplay;
    imports->getProcAddress = wrapped_getProcAddress;
    imports->getDispatchAddress = wrapped_getDispatchAddress;
    LOG("Initialized successfully");
    return EGL_TRUE;
}
