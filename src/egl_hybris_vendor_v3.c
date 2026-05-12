#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <stddef.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <glvnd/libeglabi.h>
#include <hybris/gralloc/gralloc.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>

#define LOG(fmt, ...) fprintf(stderr, "hybris_vendor: " fmt "\n", ##__VA_ARGS__)
#ifndef EGL_NATIVE_BUFFER_HYBRIS
#define EGL_NATIVE_BUFFER_HYBRIS 0x3140
#endif
#define FRAME_W 1080
#define FRAME_H 2412

typedef EGLBoolean (*PFNEGLHYBRISCREATENATIVEBUFFERPROC)(EGLint,EGLint,EGLint,EGLint,EGLint*,EGLClientBuffer*);
typedef EGLBoolean (*PFNEGLHYBRISRELEASENATIVEBUFFERPROC)(EGLClientBuffer);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum,GLeglImageOES);
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay,EGLContext,EGLenum,EGLClientBuffer,const EGLint*);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay,EGLImageKHR);

static void *hybris_lib = NULL;
static __EGLapiImports hybris_imports;
static EGLDisplay our_display = EGL_NO_DISPLAY;

static hwc2_compat_device_t *hwc2_dev = NULL;
static hwc2_compat_display_t *hwc2_disp = NULL;
static hwc2_compat_layer_t *hwc2_layer = NULL;
static struct ANativeWindow *hwc2_win = NULL;
static int hwc2_ready = 0;

static EGLClientBuffer native_buf = NULL;
static EGLImageKHR native_image = EGL_NO_IMAGE_KHR;
static GLuint render_tex = 0, render_fbo = 0;

static volatile buffer_handle_t gbm_src_handle = NULL;
static volatile int gbm_src_stride = 0;

static PFNEGLHYBRISCREATENATIVEBUFFERPROC eglHybrisCreateNativeBuffer = NULL;
static PFNEGLHYBRISRELEASENATIVEBUFFERPROC eglHybrisReleaseNativeBuffer = NULL;
static PFNEGLCREATEIMAGEKHRPROC  _eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC _eglDestroyImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC _glEGLImageTargetTexture2DOES = NULL;

static EGLBoolean (*hybris_eglGetConfigAttrib)(EGLDisplay,EGLConfig,EGLint,EGLint*) = NULL;
static EGLSurface (*hybris_eglCreatePbufferSurface)(EGLDisplay,EGLConfig,const EGLint*) = NULL;
static EGLBoolean (*hybris_eglSwapBuffers)(EGLDisplay,EGLSurface) = NULL;

/* Get HWC2 state from drmadapter platform */
static hwc2_compat_display_t *get_hwc2_disp(void) {
    static void *drmadapter_lib = NULL;
    static hwc2_compat_display_t *(*get_disp)(void) = NULL;
    if (!drmadapter_lib) {
        drmadapter_lib = dlopen("eglplatform_drmadapter.so", RTLD_NOW | RTLD_NOLOAD);
        if (!drmadapter_lib)
            drmadapter_lib = dlopen("/usr/lib/aarch64-linux-gnu/libhybris/eglplatform_drmadapter.so", RTLD_NOW);
        if (drmadapter_lib)
            get_disp = dlsym(drmadapter_lib, "drmadapter_get_hwc2_display");
    }
    return get_disp ? get_disp() : hwc2_disp;
}
static hwc2_compat_layer_t *get_hwc2_layer(void) {
    static void *drmadapter_lib = NULL;
    static hwc2_compat_layer_t *(*get_layer)(void) = NULL;
    if (!drmadapter_lib) {
        drmadapter_lib = dlopen("eglplatform_drmadapter.so", RTLD_NOW | RTLD_NOLOAD);
        if (!drmadapter_lib)
            drmadapter_lib = dlopen("/usr/lib/aarch64-linux-gnu/libhybris/eglplatform_drmadapter.so", RTLD_NOW);
        if (drmadapter_lib)
            get_layer = dlsym(drmadapter_lib, "drmadapter_get_hwc2_layer");
    }
    return get_layer ? get_layer() : hwc2_layer;
}

static void hwc2_do_present(void) {
    hwc2_compat_display_t *_disp = get_hwc2_disp();
    hwc2_compat_layer_t   *_layer = get_hwc2_layer();
    LOG("hwc2_do_present: disp=%p layer=%p nbuf=%p", (void*)_disp,(void*)_layer,(void*)native_buf);
    if (!_disp || !_layer || !native_buf) { LOG("hwc2_do_present: missing state"); return; }
    struct ANativeWindowBuffer *nb = (struct ANativeWindowBuffer*)native_buf;
    hwc2_disp = _disp; hwc2_layer = _layer;
    uint32_t nt=0, nr=0;
    hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    LOG("hwc2_do_present: validate nt=%u nr=%u", nt, nr);
    if (nt) hwc2_compat_display_accept_changes(hwc2_disp);
    hwc2_compat_layer_set_buffer(hwc2_layer, 0, nb, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, 0, nb, -1, 0);
    int32_t fence = -1;
    int err = hwc2_compat_display_present(hwc2_disp, &fence);
    LOG("hwc2_do_present: err=%d fence=%d", err, fence);
    if (fence >= 0) close(fence);
}

/* Called from GBM KMS thread - store handle only, no GL */
void gnome_mali_hwc2_present_gralloc(buffer_handle_t src_handle, int w, int h, int stride) {
    LOG("gnome_mali_hwc2_present_gralloc: handle=%p stride=%d", (void*)src_handle, stride);
    gbm_src_handle = src_handle;
    gbm_src_stride = stride;
}

/* Called from render thread - CPU memcpy GBM->native_buf then present */
static void blit_gbm_to_native_and_present(void) {
    LOG("BG1: handle=%p stride=%d nbuf=%p", (void*)gbm_src_handle, gbm_src_stride, (void*)native_buf);
    if (!gbm_src_handle) { LOG("BG1: no handle yet"); return; }
    if (!hwc2_disp || !hwc2_layer || !native_buf) { LOG("BG1: missing state"); return; }

    buffer_handle_t src_handle = (buffer_handle_t)gbm_src_handle;
    int src_stride = gbm_src_stride;

    struct ANativeWindowBuffer *nb = (struct ANativeWindowBuffer*)native_buf;
    buffer_handle_t dst_handle = nb->handle;
    int dst_stride = nb->stride;

    LOG("BG2: src=%p src_stride=%d dst=%p dst_stride=%d",
        (void*)src_handle, src_stride, (void*)dst_handle, dst_stride);

    void *src_ptr = NULL;
    int ret = hybris_gralloc_lock(src_handle, 0x1, 0, 0, FRAME_W, FRAME_H, &src_ptr);
    LOG("BG3: src lock ret=%d ptr=%p", ret, src_ptr);
    if (ret != 0 || !src_ptr) { LOG("BG3: src lock FAILED"); hwc2_do_present(); return; }

    unsigned int *px = (unsigned int*)src_ptr;
    LOG("BG3a: src sample [0,0]=%08x [center]=%08x",
        px[0], px[(FRAME_H/2)*src_stride + FRAME_W/2]);

    void *dst_ptr = NULL;
    ret = hybris_gralloc_lock(dst_handle, 0x2, 0, 0, FRAME_W, FRAME_H, &dst_ptr);
    LOG("BG4: dst lock ret=%d ptr=%p", ret, dst_ptr);
    if (ret != 0 || !dst_ptr) {
        LOG("BG4: dst lock FAILED");
        hybris_gralloc_unlock(src_handle);
        hwc2_do_present();
        return;
    }

    LOG("BG5: copying %d rows", FRAME_H);
    for (int y = 0; y < FRAME_H; y++) {
        memcpy((char*)dst_ptr + y * dst_stride * 4,
               (char*)src_ptr + y * src_stride * 4,
               FRAME_W * 4);
    }
    LOG("BG6: copy done, unlocking");

    hybris_gralloc_unlock(src_handle);
    hybris_gralloc_unlock(dst_handle);
    LOG("BG7: presenting");
    hwc2_do_present();
    LOG("BG8: done");
}

static void on_hotplug(HWC2EventListener *s,int32_t q,hwc2_display_t d,bool c,bool p){}
static void on_vsync(HWC2EventListener *s,int32_t q,hwc2_display_t d,int64_t t){}
static void on_refresh(HWC2EventListener *s,int32_t q,hwc2_display_t d){}

static void present_cb(void *ud, struct ANativeWindow *w, struct ANativeWindowBuffer *buf) {
    hwc2_do_present();
    HWCNativeBufferSetFence(buf,-1);
}

static void init_hwc2(void) {
    if (hwc2_ready) return;
    LOG("init_hwc2: starting");
    setenv("EGL_PLATFORM","hwcomposer",0);
    setenv("HYBRIS_EGLPLATFORM","hwcomposer",0);
    hybris_gralloc_initialize(0);
    hwc2_dev = hwc2_compat_device_new(false);
    if (!hwc2_dev) { LOG("init_hwc2: device_new failed"); return; }
    HWC2EventListener *l = calloc(1, sizeof(*l));
    l->on_vsync_received   = on_vsync;
    l->on_hotplug_received = on_hotplug;
    l->on_refresh_received = on_refresh;
    hwc2_compat_device_register_callback(hwc2_dev,l,0);
    hwc2_compat_device_on_hotplug(hwc2_dev,0,true);
    hwc2_disp = hwc2_compat_device_get_display_by_id(hwc2_dev,0);
    if (!hwc2_disp) { LOG("init_hwc2: no display"); return; }
    hwc2_compat_display_set_power_mode(hwc2_disp,2);
    hwc2_compat_display_set_vsync_enabled(hwc2_disp,1);
    hwc2_layer = hwc2_compat_display_create_layer(hwc2_disp);
    hwc2_compat_layer_set_composition_type(hwc2_layer,4);
    hwc2_compat_layer_set_blend_mode(hwc2_layer,1);
    hwc2_compat_layer_set_source_crop(hwc2_layer,0,0,FRAME_W,FRAME_H);
    hwc2_compat_layer_set_display_frame(hwc2_layer,0,0,FRAME_W,FRAME_H);
    hwc2_compat_layer_set_visible_region(hwc2_layer,0,0,FRAME_W,FRAME_H);
    hwc2_win = HWCNativeWindowCreate(FRAME_W,FRAME_H,1,present_cb,NULL);
    LOG("init_hwc2: done display=%p layer=%p win=%p",
        (void*)hwc2_disp,(void*)hwc2_layer,(void*)hwc2_win);
    hwc2_ready = 1;
}

static void init_native_buffer(EGLDisplay dpy) {
    if (native_buf) return;
    LOG("init_native_buffer: dpy=%p", (void*)dpy);
    if (!eglHybrisCreateNativeBuffer) {
        eglHybrisCreateNativeBuffer   = hybris_imports.getProcAddress("eglHybrisCreateNativeBuffer");
        eglHybrisReleaseNativeBuffer  = hybris_imports.getProcAddress("eglHybrisReleaseNativeBuffer");
        _eglCreateImageKHR            = hybris_imports.getProcAddress("eglCreateImageKHR");
        _eglDestroyImageKHR           = hybris_imports.getProcAddress("eglDestroyImageKHR");
        _glEGLImageTargetTexture2DOES = hybris_imports.getProcAddress("glEGLImageTargetTexture2DOES");
        LOG("init_native_buffer: hybrisCreate=%p eglCI=%p glImg=%p",
            (void*)eglHybrisCreateNativeBuffer,(void*)_eglCreateImageKHR,
            (void*)_glEGLImageTargetTexture2DOES);
    }
    if (!eglHybrisCreateNativeBuffer || !_eglCreateImageKHR) {
        LOG("init_native_buffer: missing extensions"); return;
    }
    EGLint stride=0;
    EGLBoolean ok = eglHybrisCreateNativeBuffer(FRAME_W,FRAME_H,0xB00,1,&stride,&native_buf);
    LOG("init_native_buffer: ok=%d buf=%p stride=%d", ok, (void*)native_buf, stride);
    if (!ok || !native_buf) { LOG("init_native_buffer: FAILED"); return; }

    native_image = _eglCreateImageKHR(dpy,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,native_buf,NULL);
    LOG("init_native_buffer: native_image=%p err=0x%x", (void*)native_image, eglGetError());

    glGenTextures(1,&render_tex);
    glBindTexture(GL_TEXTURE_2D,render_tex);
    _glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,(GLeglImageOES)native_image);
    glGenFramebuffers(1,&render_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER,render_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,render_tex,0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    LOG("init_native_buffer: render_tex=%u render_fbo=%u status=0x%x complete=%d",
        render_tex, render_fbo, status, status==0x8CD5);
    glBindFramebuffer(GL_FRAMEBUFFER,0);

    /* Log native_buf fields for reference */
    struct ANativeWindowBuffer *nb = (struct ANativeWindowBuffer*)native_buf;
    LOG("init_native_buffer: nb handle=%p stride=%d fmt=%d usage=%d",
        (void*)nb->handle, nb->stride, nb->format, (int)nb->usage);
    LOG("init_native_buffer: done");
}

static EGLDisplay wrapped_getPlatformDisplay(EGLenum platform, void *nativeDisplay,
                                              const EGLAttrib *attrib_list) {
    LOG("getPlatformDisplay platform=0x%x native=%p", platform, nativeDisplay);
    fprintf(stderr, "hybris_vendor: getPlatformDisplay ENTRY platform=0x%x native=%p\n", platform, (void*)nativeDisplay); fflush(stderr);
    if (platform == EGL_PLATFORM_GBM_KHR) {
        EGLDisplay dpy = hybris_imports.getPlatformDisplay(EGL_NONE,EGL_DEFAULT_DISPLAY,NULL);
        our_display = dpy;
        LOG("getPlatformDisplay: our_display=%p (GBM)", (void*)dpy);
        pthread_t tid;
        // init_hwc2 now handled by drmadapter platform
        pthread_detach(tid);
        return dpy;
    }
    EGLDisplay dpy = hybris_imports.getPlatformDisplay(platform,nativeDisplay,attrib_list);
    LOG("getPlatformDisplay: other display=%p", (void*)dpy);
    return dpy;
}

static EGLSurface our_native_buf_surface = EGL_NO_SURFACE;
#define MAX_OUR_SURFACES 8
static EGLSurface our_surfaces[MAX_OUR_SURFACES];
static int our_surfaces_n = 0;
static int is_our_surface(EGLSurface s) {
    for (int i=0;i<our_surfaces_n;i++) if (our_surfaces[i]==s) return 1;
    return 0;
}

static EGLSurface my_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                              EGLNativeWindowType win,
                                              const EGLint *attrib_list) {
    LOG("my_eglCreateWindowSurface dpy=%p our=%p win=%p", (void*)dpy, (void*)our_display, (void*)win);
    if (our_display != EGL_NO_DISPLAY && dpy == our_display) {
        /* Create pbuffer from native_buf so mutter renders directly into it */
        if (!native_buf) init_native_buffer(dpy);
        if (native_buf) {
            typedef EGLSurface (*eglCreatePbufferFromClientBuffer_t)(EGLDisplay,EGLenum,EGLClientBuffer,EGLConfig,const EGLint*);
            eglCreatePbufferFromClientBuffer_t fn =
                hybris_imports.getProcAddress("eglCreatePbufferFromClientBuffer");
            LOG("my_eglCreateWindowSurface: eglCreatePbufferFromClientBuffer=%p", (void*)fn);
            if (fn) {
                EGLint attribs[] = {EGL_NONE};
                EGLSurface s = fn(dpy, EGL_NATIVE_BUFFER_HYBRIS, native_buf, config, attribs);
                LOG("my_eglCreateWindowSurface: native_buf surface=%p err=0x%x", (void*)s, eglGetError());
                if (s != EGL_NO_SURFACE) {
                    our_native_buf_surface = s;
                    return s;
                }
            }
        }
        /* Fallback: plain pbuffer */
        LOG("my_eglCreateWindowSurface: fallback to plain pbuffer");
        EGLint pbuf_attribs[] = {EGL_WIDTH,FRAME_W,EGL_HEIGHT,FRAME_H,EGL_NONE};
        EGLSurface s = hybris_eglCreatePbufferSurface(dpy,config,pbuf_attribs);
        LOG("my_eglCreateWindowSurface: pbuffer=%p", (void*)s);
        if (s && our_surfaces_n < MAX_OUR_SURFACES) our_surfaces[our_surfaces_n++] = s;
        return s;
    }
    typedef EGLSurface (*fn_t)(EGLDisplay,EGLConfig,EGLNativeWindowType,const EGLint*);
    fn_t real = (fn_t)hybris_imports.getProcAddress("eglCreateWindowSurface");
    return real ? real(dpy,config,win,attrib_list) : EGL_NO_SURFACE;
}

static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (our_display != EGL_NO_DISPLAY && dpy == our_display && is_our_surface(surface)) {
        LOG("my_eglSwapBuffers: our surface init=%s",
            native_buf?"yes":"no");
        if (!native_buf) init_native_buffer(dpy);
        /* GPU blit: pbuffer (FBO 0) -> render_fbo (native_buf) via glBlitFramebuffer */
        typedef void (*glBindFramebuffer_t)(GLenum,GLuint);
        typedef void (*glBlitFramebuffer_t)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
        typedef void (*glFinish_t)(void);
        static glBindFramebuffer_t _glBF = NULL;
        static glBlitFramebuffer_t _glBlit = NULL;
        static glFinish_t _glFin = NULL;
        if (!_glBF)   _glBF   = hybris_imports.getProcAddress("glBindFramebuffer");
        if (!_glBlit) _glBlit = hybris_imports.getProcAddress("glBlitFramebuffer");
        if (!_glFin)  _glFin  = hybris_imports.getProcAddress("glFinish");
        LOG("my_eglSwapBuffers: glBlit=%p render_fbo=%u", (void*)_glBlit, render_fbo);
        if (_glBF && _glBlit && render_fbo) {
            /* Bind pbuffer as read source (it IS FBO 0 on current context) */
            _glBF(0x8CA8/*GL_READ_FRAMEBUFFER*/, 0);
            _glBF(0x8CA9/*GL_DRAW_FRAMEBUFFER*/, render_fbo);
            LOG("my_eglSwapBuffers: calling glBlitFramebuffer");
            /* Flip Y only (OpenGL bottom-up -> display top-down) */
            _glBlit(0, FRAME_H, FRAME_W, 0,
                    0, 0, FRAME_W, FRAME_H,
                    0x00004000/*GL_COLOR_BUFFER_BIT*/, 0x2600/*GL_NEAREST*/);
            LOG("my_eglSwapBuffers: blit done, finish");
            if (_glFin) _glFin();
            /* Restore default FBO */
            _glBF(0x8D40/*GL_FRAMEBUFFER*/, 0);
            LOG("my_eglSwapBuffers: restored FBO");
        }
        hwc2_do_present();
        return EGL_TRUE;
    }
    typedef EGLBoolean (*fn_t)(EGLDisplay,EGLSurface);
    fn_t real = hybris_imports.getProcAddress ? (fn_t)hybris_imports.getProcAddress("eglSwapBuffers") : NULL;
    return real ? real(dpy,surface) : EGL_TRUE;
}

static EGLBoolean my_eglSwapBuffersWithDamage(EGLDisplay dpy, EGLSurface surface,
                                               const EGLint *rects, EGLint n_rects) {
    return my_eglSwapBuffers(dpy, surface);
}

static EGLBoolean my_eglGetConfigAttrib(EGLDisplay dpy,EGLConfig config,
                                          EGLint attribute,EGLint *value) {
    EGLBoolean ret = hybris_eglGetConfigAttrib(dpy,config,attribute,value);
    if (ret && attribute == EGL_NATIVE_VISUAL_ID) {
        EGLint alpha=0;
        hybris_eglGetConfigAttrib(dpy,config,EGL_ALPHA_SIZE,&alpha);
        switch(*value) {
            case 1: *value = alpha>0 ? 0x34324241 : 0x34324258; break;
            case 5: *value = alpha>0 ? 0x34325241 : 0x34325258; break;
            case 4: *value = 0x36314752; break;
            default: *value = 0x34324258; break;
        }
    }
    return ret;
}



/* Fake EGLDeviceEXT for our HWC2 device */
static void *our_egl_device = (void*)0xdeadbeef;

static EGLBoolean my_eglQueryDevicesEXT(EGLint max_devices, void **devices, EGLint *num_devices) {
    LOG("my_eglQueryDevicesEXT max=%d", max_devices);
    if (num_devices) *num_devices = 1;
    if (devices && max_devices > 0) devices[0] = our_egl_device;
    return EGL_TRUE;
}

static EGLDisplay my_eglGetPlatformDisplayEXT(EGLenum platform, void *native, const int *attribs) {
    LOG("my_eglGetPlatformDisplayEXT platform=0x%x native=%p", platform, native);
    if (platform == 0x31D7 /* EGL_PLATFORM_GBM_KHR */ || native != NULL) {
        EGLDisplay dpy = hybris_imports.getPlatformDisplay(EGL_NONE, EGL_DEFAULT_DISPLAY, NULL);
        our_display = dpy;
        LOG("my_eglGetPlatformDisplayEXT: our_display=%p", (void*)dpy);
        pthread_t tid;
        // init_hwc2 now handled by drmadapter platform
        pthread_detach(tid);
        return dpy;
    }
    return EGL_NO_DISPLAY;
}

static void *wrapped_getProcAddress(const char *name) {
    if (!strcmp(name,"eglGetConfigAttrib")) return (void*)my_eglGetConfigAttrib;
    if (!strcmp(name,"eglCreateWindowSurface")) return (void*)my_eglCreateWindowSurface;
    if (!strcmp(name,"eglSwapBuffers")) return (void*)my_eglSwapBuffers;
    if (strstr(name,"SwapBuffersWithDamage")) return (void*)my_eglSwapBuffersWithDamage;
    if (!strcmp(name,"eglGetPlatformDisplayEXT")) return (void*)my_eglGetPlatformDisplayEXT;
    if (!strcmp(name,"eglQueryDevicesEXT")) return (void*)my_eglQueryDevicesEXT;
    LOG("getProcAddress: %s", name);
    return hybris_imports.getProcAddress ? hybris_imports.getProcAddress(name) : NULL;
}


/* Dummy function for unknown GLVND 1.7.0 field at imports+88
 * Must be non-NULL for findNativeDisplayPlatform to be registered */
EGLBoolean eglvnd_unknown_field_88(void) {
    LOG("eglvnd_unknown_field_88 called");
    return EGL_TRUE;
}
EGLenum findNativeDisplayPlatform(void *native_display) {
    LOG("findNativeDisplayPlatform: native=%p", native_display); fprintf(stderr, "hybris_vendor: findNativeDisplayPlatform ENTRY native=%p\n", native_display); fflush(stderr);
    if (native_display != NULL) {
        LOG("findNativeDisplayPlatform: claiming %p as GBM", native_display);
        return 0x31D7; /* EGL_PLATFORM_GBM_KHR */
    }
    return EGL_NONE;
}

static void *wrapped_getDispatchAddress(const char *name) {
    if (!strcmp(name,"eglGetConfigAttrib")) return (void*)my_eglGetConfigAttrib;
    if (!strcmp(name,"eglCreateWindowSurface")) return (void*)my_eglCreateWindowSurface;
    if (!strcmp(name,"eglSwapBuffers")) return (void*)my_eglSwapBuffers;
    if (strstr(name,"SwapBuffersWithDamage")) return (void*)my_eglSwapBuffersWithDamage;
    if (!strcmp(name,"eglGetPlatformDisplayEXT")) return (void*)my_eglGetPlatformDisplayEXT;
    if (!strcmp(name,"eglQueryDevicesEXT")) return (void*)my_eglQueryDevicesEXT;
    LOG("getProcAddress: %s", name);
    return hybris_imports.getDispatchAddress ? hybris_imports.getDispatchAddress(name) : NULL;
}

EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports *exports,
                       __EGLvendorInfo *vendor, __EGLapiImports *imports) {
    LOG("__egl_Main version=0x%x", version);
    hybris_lib = dlopen("libEGL_libhybris.so.0",RTLD_NOW|RTLD_GLOBAL);
    if (!hybris_lib) { LOG("__egl_Main: dlopen failed: %s",dlerror()); return EGL_FALSE; }
    typedef EGLBoolean (*egl_main_t)(uint32_t,const __EGLapiExports*,__EGLvendorInfo*,__EGLapiImports*);
    egl_main_t hybris_egl_main = dlsym(hybris_lib,"__egl_Main");
    if (!hybris_egl_main) { LOG("__egl_Main: no __egl_Main in hybris"); return EGL_FALSE; }
    memset(&hybris_imports,0,sizeof(hybris_imports));
    if (!hybris_egl_main(version,exports,vendor,&hybris_imports)) {
        LOG("__egl_Main: hybris failed"); return EGL_FALSE;
    }
    if (hybris_imports.getProcAddress) {
        hybris_eglGetConfigAttrib      = hybris_imports.getProcAddress("eglGetConfigAttrib");
        hybris_eglCreatePbufferSurface = hybris_imports.getProcAddress("eglCreatePbufferSurface");
        hybris_eglSwapBuffers          = hybris_imports.getProcAddress("eglSwapBuffers");
        LOG("__egl_Main: CA=%p PBS=%p SB=%p",
            (void*)hybris_eglGetConfigAttrib,(void*)hybris_eglCreatePbufferSurface,
            (void*)hybris_eglSwapBuffers);
    }
    *imports = hybris_imports;
    imports->getPlatformDisplay       = wrapped_getPlatformDisplay;
    imports->getProcAddress           = wrapped_getProcAddress;
    imports->getDispatchAddress       = wrapped_getDispatchAddress;
    /* Pad to match actual GLVND 1.7.0 binary struct layout
     * Binary expects findNativeDisplayPlatform at offset 112 (field 14)
     * Our header has it at offset 80 (field 10) - need 3 more fields
     * Set dummy pointers for fields 11, 12, 13 */
    void **imports_raw = (void**)imports;
    /* field 11 = offset 88 */ imports_raw[11] = NULL;
    /* field 12 = offset 96 */ imports_raw[12] = NULL;
    /* field 13 = offset 104 */ imports_raw[13] = NULL;
    /* field 14 = offset 112 */ imports_raw[14] = (void*)findNativeDisplayPlatform;
    LOG("__egl_Main: imports=%p writing to %p (offset 112)", (void*)imports, (void*)&imports_raw[14]);
    LOG("__egl_Main: set findNativeDisplayPlatform at raw offset 112");
    LOG("__egl_Main: findNativeDisplayPlatform=%p getPlatformDisplay=%p", (void*)findNativeDisplayPlatform, (void*)wrapped_getPlatformDisplay);
    imports->findNativeDisplayPlatform = findNativeDisplayPlatform;
    /* Set non-NULL at imports+88 so GLVND 1.7.0 registers findNativeDisplayPlatform */
    *((void**)((char*)imports + 88)) = (void*)eglvnd_unknown_field_88;
    LOG("__egl_Main: AFTER ASSIGN: &imports->findNativeDisplayPlatform = %p value=%p", (void*)&imports->findNativeDisplayPlatform, (void*)imports->findNativeDisplayPlatform);
    /* Also write directly to memory */
    *((void**)((char*)imports + 80)) = (void*)findNativeDisplayPlatform;
    LOG("__egl_Main: AFTER RAW WRITE +80: value=%p", *((void**)((char*)imports + 80)));
    LOG("__egl_Main: &imports->findNativeDisplayPlatform = %p value=%p", (void*)&imports->findNativeDisplayPlatform, (void*)imports->findNativeDisplayPlatform);
    LOG("__egl_Main: imports base=%p offset=%zu", (void*)imports, offsetof(__EGLapiImports, findNativeDisplayPlatform));
    /* Verify what GLVND will read at vendor+112 */
    /* vendor = imports - 0x20, so vendor+112 = imports+112-0x20 = imports+80 */
    void *vendor_base = (char*)imports - 0x20;
    void **at_112 = (void**)((char*)vendor_base + 112);
    LOG("__egl_Main: vendor_base=%p vendor+112=%p (should be findNativeDisplayPlatform=%p)", 
        vendor_base, *at_112, (void*)findNativeDisplayPlatform);
    LOG("__egl_Main: Initialized successfully");
    return EGL_TRUE;
}
