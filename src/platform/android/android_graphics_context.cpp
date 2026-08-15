#include "platform/android/android_graphics_context.hpp"

#include <iostream>
#include <vector>
#include <cstring>

namespace lcl::platform::android {

AndroidGraphicsContext::AndroidGraphicsContext() = default;

AndroidGraphicsContext::~AndroidGraphicsContext() {
    shutdown();
}

bool AndroidGraphicsContext::initialize(uint32_t width, uint32_t height) {
    if (m_initialized) return true;

    m_width = width > 0 ? width : 320;
    m_height = height > 0 ? height : 640;

    // 1. Get default EGL display
    m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        std::cerr << "[AndroidGraphicsContext] Failed to get EGLDisplay\n";
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_eglDisplay, &major, &minor)) {
        std::cerr << "[AndroidGraphicsContext] Failed to initialize EGL\n";
        return false;
    }

    // 2. Choose EGL config
    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    if (!eglChooseConfig(m_eglDisplay, configAttribs, &m_eglConfig, 1, &numConfigs) || numConfigs < 1) {
        std::cerr << "[AndroidGraphicsContext] Failed to choose matching EGLConfig\n";
        return false;
    }

    // 3. Create PBuffer surface
    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, static_cast<EGLint>(m_width),
        EGL_HEIGHT, static_cast<EGLint>(m_height),
        EGL_NONE
    };
    m_eglSurface = eglCreatePbufferSurface(m_eglDisplay, m_eglConfig, pbufferAttribs);
    if (m_eglSurface == EGL_NO_SURFACE) {
        std::cerr << "[AndroidGraphicsContext] Failed to create EGL PBuffer surface\n";
        return false;
    }

    // 4. Create OpenGL ES 3.0 Context
    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    m_eglContext = eglCreateContext(m_eglDisplay, m_eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (m_eglContext == EGL_NO_CONTEXT) {
        std::cerr << "[AndroidGraphicsContext] Failed to create EGLContext (GLES 3)\n";
        return false;
    }

    // 5. Make context current
    if (!makeCurrent()) {
        std::cerr << "[AndroidGraphicsContext] Failed to make EGL context current\n";
        return false;
    }

    // 6. Discover extensions
    const char* eglExts = eglQueryString(m_eglDisplay, EGL_EXTENSIONS);
    if (eglExts) {
        m_hasAhbExtension = std::strstr(eglExts, "EGL_ANDROID_get_native_client_buffer") != nullptr;
        m_hasImageExtension = std::strstr(eglExts, "EGL_KHR_image_base") != nullptr;
    }

    m_initialized = true;
    return true;
}

void AndroidGraphicsContext::shutdown() {
    if (!m_initialized) return;

    if (m_eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (m_eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(m_eglDisplay, m_eglContext);
            m_eglContext = EGL_NO_CONTEXT;
        }

        if (m_eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(m_eglDisplay, m_eglSurface);
            m_eglSurface = EGL_NO_SURFACE;
        }

        eglTerminate(m_eglDisplay);
        m_eglDisplay = EGL_NO_DISPLAY;
    }

    m_initialized = false;
}

bool AndroidGraphicsContext::makeCurrent() {
    if (m_eglDisplay == EGL_NO_DISPLAY || m_eglContext == EGL_NO_CONTEXT) {
        return false;
    }
    return eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext) == EGL_TRUE;
}

bool AndroidGraphicsContext::resize(uint32_t width, uint32_t height) {
    if (!m_initialized) return false;
    if (width == m_width && height == m_height) return true;

    m_width = width;
    m_height = height;

    if (m_eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(m_eglDisplay, m_eglSurface);
        m_eglSurface = EGL_NO_SURFACE;
    }

    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, static_cast<EGLint>(m_width),
        EGL_HEIGHT, static_cast<EGLint>(m_height),
        EGL_NONE
    };
    m_eglSurface = eglCreatePbufferSurface(m_eglDisplay, m_eglConfig, pbufferAttribs);
    return makeCurrent();
}

bool AndroidGraphicsContext::present() {
    if (!m_initialized) return false;
    glFlush();
    return true;
}

bool AndroidGraphicsContext::readback(uint32_t* destination, uint32_t width, uint32_t height) {
    if (!m_initialized || !destination) return false;
    makeCurrent();
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, destination);
    return true;
}

lcl::platform::TextureHandle AndroidGraphicsContext::importTexture(const lcl::platform::INativeBuffer& /*buffer*/) {
    // Buffer texture import will be connected with AHardwareBuffer in the next stage
    return lcl::platform::kInvalidTextureHandle;
}

void AndroidGraphicsContext::releaseTexture(lcl::platform::TextureHandle texture) {
    if (texture != lcl::platform::kInvalidTextureHandle) {
        GLuint tex = static_cast<GLuint>(texture);
        glDeleteTextures(1, &tex);
    }
}

} // namespace lcl::platform::android
