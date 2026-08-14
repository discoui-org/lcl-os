#include "render/client_egl_context.hpp"

#include <GLES2/gl2.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace lcl::render {
namespace {

int openRenderNode() {
    for (int index = 128; index <= 143; ++index) {
        const std::string path = "/dev/dri/renderD" + std::to_string(index);
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

bool unavailable(const std::string& reason) {
    // This is emitted once per app at startup, so it remains useful during
    // QEMU diagnosis without becoming a per-frame trace source.
    std::cerr << "[LCL Canvas] Client GPU unavailable: " << reason << "\n";
    return false;
}

bool hasExtension(const char* extensions, const char* wanted) {
    if (!extensions || !wanted || *wanted == '\0') return false;
    const std::string all(extensions);
    const std::string needle(wanted);
    size_t start = 0;
    while (start < all.size()) {
        const size_t end = all.find(' ', start);
        const size_t length = (end == std::string::npos ? all.size() : end) - start;
        if (length == needle.size() && all.compare(start, length, needle) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

} // namespace

ClientEGLContext::~ClientEGLContext() {
    shutdown();
}

bool ClientEGLContext::isSoftwareRenderer(const char* renderer) {
    if (!renderer) return true;
    std::string value(renderer);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.find("llvmpipe") != std::string::npos ||
           value.find("softpipe") != std::string::npos ||
           value.find("swrast") != std::string::npos ||
           value.find("software") != std::string::npos;
}

bool ClientEGLContext::createSurface(uint32_t width, uint32_t height) {
    const EGLint attributes[] = {
        EGL_WIDTH, static_cast<EGLint>(width),
        EGL_HEIGHT, static_cast<EGLint>(height),
        EGL_NONE,
    };
    m_surface = eglCreatePbufferSurface(m_display, m_config, attributes);
    if (m_surface == EGL_NO_SURFACE) {
        return false;
    }
    m_width = width;
    m_height = height;
    return makeCurrent();
}

bool ClientEGLContext::initialize(uint32_t width, uint32_t height) {
    if (m_initialized) return resize(width, height);
    if (width == 0 || height == 0) return unavailable("invalid surface size");

    m_renderFd = openRenderNode();
    if (m_renderFd < 0) return unavailable("no accessible /dev/dri/renderD* node");
    m_gbmDevice = gbm_create_device(m_renderFd);
    if (!m_gbmDevice) {
        shutdown();
        return unavailable("gbm_create_device failed");
    }

    const auto getPlatformDisplayExt = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    const auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYPROC>(
        eglGetProcAddress("eglGetPlatformDisplay"));
    const EGLint emptyAttributes[] = {EGL_NONE};
    const EGLAttrib emptyAttributesModern[] = {EGL_NONE};
    const auto tryDisplay = [this](EGLDisplay display) {
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return false;
        m_display = display;
        return true;
    };
    bool displayReady = false;
    if (getPlatformDisplayExt) {
        displayReady = tryDisplay(getPlatformDisplayExt(EGL_PLATFORM_GBM_KHR, m_gbmDevice, emptyAttributes));
    }
    if (!displayReady && getPlatformDisplay) {
        displayReady = tryDisplay(getPlatformDisplay(EGL_PLATFORM_GBM_KHR, m_gbmDevice, emptyAttributesModern));
    }
    if (!displayReady) {
        displayReady = tryDisplay(eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(m_gbmDevice)));
    }
    if (!displayReady) {
        shutdown();
        return unavailable("could not initialize EGL on the render node");
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        shutdown();
        return unavailable("could not bind the OpenGL ES API");
    }
    // The renderer draws exclusively into its own FBO. VirGL's GBM EGL
    // implementation exposes no pbuffer configs, but it does expose the
    // standard surfaceless extension required for exactly this use case.
    m_surfaceless = hasExtension(eglQueryString(m_display, EGL_EXTENSIONS),
                                 "EGL_KHR_surfaceless_context");
    const EGLint surfacelessConfigAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    const EGLint pbufferConfigAttributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint configCount = 0;
    const EGLint* configAttributes = m_surfaceless
        ? surfacelessConfigAttributes
        : pbufferConfigAttributes;
    if (!eglChooseConfig(m_display, configAttributes, &m_config, 1, &configCount) || configCount != 1) {
        shutdown();
        return unavailable(m_surfaceless
            ? "no RGBA OpenGL ES configuration for a surfaceless context"
            : "no RGBA OpenGL ES pbuffer configuration");
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttributes);
    if (m_context == EGL_NO_CONTEXT) {
        shutdown();
        return unavailable("could not create the OpenGL ES context");
    }
    m_width = width;
    m_height = height;
    const bool targetReady = m_surfaceless ? makeCurrent() : createSurface(width, height);
    if (!targetReady) {
        shutdown();
        return unavailable(m_surfaceless
            ? "could not make the surfaceless OpenGL ES context current"
            : "could not create the OpenGL ES pbuffer context");
    }

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    m_rendererString = renderer ? renderer : "unknown";
    m_hardwareAccelerated = !isSoftwareRenderer(renderer);
    if (!m_hardwareAccelerated) {
        shutdown();
        return unavailable("software GL renderer (" + m_rendererString + ")");
    }

    m_initialized = true;
    // Native apps inherit sessiond's pipe-backed stdout.  Write the capability
    // line to the unbuffered diagnostic stream so it is visible at startup,
    // rather than only when a later trace happens to flush stdout.
    std::cerr << "[LCL Canvas] Client GPU backend active (" << m_rendererString << ")\n";
    return true;
}

bool ClientEGLContext::makeCurrent() {
    if (m_display == EGL_NO_DISPLAY || m_context == EGL_NO_CONTEXT) return false;
    if (m_surfaceless) {
        return eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) == EGL_TRUE;
    }
    return m_surface != EGL_NO_SURFACE &&
           eglMakeCurrent(m_display, m_surface, m_surface, m_context) == EGL_TRUE;
}

bool ClientEGLContext::resize(uint32_t width, uint32_t height) {
    if (!m_initialized) return false;
    if (width == m_width && height == m_height) return true;
    if (width == 0 || height == 0) return false;
    if (m_surfaceless) {
        m_width = width;
        m_height = height;
        return makeCurrent();
    }
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(m_display, m_surface);
    m_surface = EGL_NO_SURFACE;
    return createSurface(width, height);
}

bool ClientEGLContext::readback(uint32_t* destination, uint32_t width, uint32_t height) {
    if (!destination || !m_initialized || width != m_width || height != m_height || !makeCurrent()) return false;

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u);
    glFinish();
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = rgba.data() + static_cast<size_t>(height - 1u - y) * width * 4u;
        uint32_t* target = destination + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = source + static_cast<size_t>(x) * 4u;
            target[x] = (static_cast<uint32_t>(pixel[3]) << 24u) |
                        (static_cast<uint32_t>(pixel[0]) << 16u) |
                        (static_cast<uint32_t>(pixel[1]) << 8u) |
                        static_cast<uint32_t>(pixel[2]);
        }
    }
    return glGetError() == GL_NO_ERROR;
}

void ClientEGLContext::shutdown() {
    if (m_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface != EGL_NO_SURFACE) eglDestroySurface(m_display, m_surface);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }
    m_surface = EGL_NO_SURFACE;
    m_context = EGL_NO_CONTEXT;
    m_display = EGL_NO_DISPLAY;
    if (m_gbmDevice) gbm_device_destroy(m_gbmDevice);
    m_gbmDevice = nullptr;
    if (m_renderFd >= 0) close(m_renderFd);
    m_renderFd = -1;
    m_width = 0;
    m_height = 0;
    m_initialized = false;
    m_hardwareAccelerated = false;
}

} // namespace lcl::render
