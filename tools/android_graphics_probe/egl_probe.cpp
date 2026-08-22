#include "egl_probe.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <iostream>

namespace lcl::probe {

EglGlesInfo runEglGlesProbe() {
    EglGlesInfo info{};

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        std::cerr << "[Probe EGL] Failed to get default EGL display.\n";
        return info;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        std::cerr << "[Probe EGL] Failed to initialize EGL display (Error: "
                  << eglGetError() << ")\n";
        return info;
    }

    info.eglVendor = eglQueryString(display, EGL_VENDOR);
    info.eglVersion = eglQueryString(display, EGL_VERSION);
    info.eglClientApis = eglQueryString(display, EGL_CLIENT_APIS);
    info.eglExtensions = eglQueryString(display, EGL_EXTENSIONS);

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
    if (!eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) {
        std::cerr << "[Probe EGL] Failed to choose EGL config for ES3 (Error: "
                  << eglGetError() << ")\n";
        eglTerminate(display);
        return info;
    }

    const EGLint pbufferAttribs[] = {
        EGL_WIDTH,  64,
        EGL_HEIGHT, 64,
        EGL_NONE
    };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
    if (surface == EGL_NO_SURFACE) {
        std::cerr << "[Probe EGL] Failed to create Pbuffer surface (Error: "
                  << eglGetError() << ")\n";
        eglTerminate(display);
        return info;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        std::cerr << "[Probe EGL] Failed to create EGL ES3 context (Error: "
                  << eglGetError() << ")\n";
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return info;
    }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        std::cerr << "[Probe EGL] Failed to make EGL context current (Error: "
                  << eglGetError() << ")\n";
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return info;
    }

    info.glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    info.glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    info.glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    info.glShadingLanguageVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    info.glExtensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    info.isSupported = true;

    // Clean up temporary context/surface
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);

    return info;
}

} // namespace lcl::probe
