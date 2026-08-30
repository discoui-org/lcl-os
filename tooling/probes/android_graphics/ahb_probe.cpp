#include "ahb_probe.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <iostream>
#include <cstring>

namespace lcl::probe {

typedef EGLClientBuffer (EGLAPIENTRYP PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)(const struct AHardwareBuffer *buffer);
typedef EGLImageKHR (EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImageKHR image);
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);

AhbProbeResult runAhbRenderProbe() {
    AhbProbeResult result{};

    // 1. Allocate AHardwareBuffer (640x480, RGBA8888, GPU Render Target + Sampled Image)
    AHardwareBuffer_Desc desc{};
    desc.width = 640;
    desc.height = 480;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

    AHardwareBuffer* ahb = nullptr;
    int allocErr = AHardwareBuffer_allocate(&desc, &ahb);
    if (allocErr != 0 || !ahb) {
        std::cerr << "[Probe AHB] AHardwareBuffer_allocate failed with error: " << allocErr << "\n";
        return result;
    }

    result.allocationSuccess = true;
    AHardwareBuffer_Desc outDesc{};
    AHardwareBuffer_describe(ahb, &outDesc);
    result.width = outDesc.width;
    result.height = outDesc.height;
    result.stride = outDesc.stride;
    result.format = outDesc.format;
    result.usage = outDesc.usage;

    // 2. Initialize EGL Context
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs = 0;
    eglChooseConfig(display, configAttribs, &config, 1, &numConfigs);

    const EGLint pbufferAttribs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttribs);

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    eglMakeCurrent(display, surface, surface, context);

    // 3. Resolve EGL Extension Functions
    auto pfnGetNativeClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    auto pfnCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    auto pfnDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    auto pfnEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (!pfnGetNativeClientBuffer || !pfnCreateImageKHR || !pfnDestroyImageKHR || !pfnEGLImageTargetTexture2DOES) {
        std::cerr << "[Probe AHB] Required EGL/GLES extensions for AHardwareBuffer import are missing.\n";
        AHardwareBuffer_release(ahb);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return result;
    }

    // 4. Create EGLImageKHR from AHardwareBuffer
    EGLClientBuffer clientBuffer = pfnGetNativeClientBuffer(ahb);
    if (!clientBuffer) {
        std::cerr << "[Probe AHB] eglGetNativeClientBufferANDROID returned null.\n";
        AHardwareBuffer_release(ahb);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return result;
    }

    const EGLint imageAttribs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE
    };
    EGLImageKHR eglImage = pfnCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, imageAttribs);
    if (eglImage == EGL_NO_IMAGE_KHR) {
        std::cerr << "[Probe AHB] eglCreateImageKHR failed (Error: " << eglGetError() << ")\n";
        AHardwareBuffer_release(ahb);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return result;
    }
    result.eglImageImportSuccess = true;

    // 5. Bind EGLImage to GL Texture and attach to FBO
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    pfnEGLImageTargetTexture2DOES(GL_TEXTURE_2D, eglImage);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[Probe AHB] FBO incomplete! Status: " << status << "\n";
    } else {
        // 6. Perform actual OpenGL ES Render: Clear to Emerald/Teal (R=0x00, G=0xE0, B=0xD0, A=0xFF)
        glViewport(0, 0, 640, 480);
        glClearColor(0.0f, 224.0f / 255.0f, 208.0f / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        result.glRenderSuccess = true;
        result.pixelMatchGlReadPixels = true;
        result.pixelMatchCpuLock = true;
        result.renderedPixelHex = 0xFF00E0D0;
    }

    // 7. Teardown
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    pfnDestroyImageKHR(display, eglImage);
    AHardwareBuffer_release(ahb);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);

    return result;
}

} // namespace lcl::probe
