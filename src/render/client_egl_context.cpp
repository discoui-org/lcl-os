#include "render/client_egl_context.hpp"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#endif

namespace lcl::render {
namespace {

#if !defined(__ANDROID__)
int openRenderNode() {
    for (int index = 128; index <= 143; ++index) {
        const std::string path = "/dev/dri/renderD" + std::to_string(index);
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}
#endif

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
#if defined(__ANDROID__)
    if (m_initialized) return resize(width, height);
    if (width == 0 || height == 0) return unavailable("invalid surface size");
    m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, nullptr, nullptr)) {
        shutdown();
        return unavailable("could not initialize Android EGL");
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        shutdown();
        return unavailable("could not bind the OpenGL ES API");
    }
#else
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
#endif
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
    // Exporting a GBM buffer is optional. Failure leaves the already verified
    // GPU-to-SHM path intact instead of disabling client GPU drawing entirely.
    createDmaBufPool(width, height);
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
        destroyDmaBufPool();
        m_width = width;
        m_height = height;
        if (!makeCurrent()) return false;
        createDmaBufPool(width, height);
        return true;
    }
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(m_display, m_surface);
    m_surface = EGL_NO_SURFACE;
    if (!createSurface(width, height)) return false;
    destroyDmaBufPool();
    createDmaBufPool(width, height);
    return true;
}

bool ClientEGLContext::createDmaBufPool(uint32_t width, uint32_t height) {
    destroyDmaBufPool();
    return appendDmaBufPool(width, height);
}

bool ClientEGLContext::appendDmaBufPool(uint32_t width, uint32_t height) {
#if defined(__ANDROID__)
    if (!m_initialized || width == 0 || height == 0 || !makeCurrent()) return false;

    using GetNativeClientBuffer = EGLClientBuffer (*)(const AHardwareBuffer*);
    const auto getNativeClientBuffer = reinterpret_cast<GetNativeClientBuffer>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    const auto imageTarget = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!getNativeClientBuffer || !createImage || !destroyImage || !imageTarget) return false;

    for (size_t index = m_dmaBufs.size(); index > 0; --index) {
        auto& slot = m_dmaBufs[index - 1];
        if (slot.busy) {
            slot.retired = true;
        } else {
            destroyDmaBufSlot(slot);
            m_dmaBufs.erase(m_dmaBufs.begin() + static_cast<std::ptrdiff_t>(index - 1));
        }
    }
    const size_t retainedPoolSize = m_dmaBufs.size();
    for (uint32_t index = 0; index < 3; ++index) {
        DmaBufSlot slot{};
        slot.id = m_nextDmaBufId++;
        slot.width = width;
        slot.height = height;
        AHardwareBuffer_Desc desc{};
        desc.width = width;
        desc.height = height;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&desc, &slot.ahb) != 0 || !slot.ahb) {
            while (m_dmaBufs.size() > retainedPoolSize) {
                destroyDmaBufSlot(m_dmaBufs.back());
                m_dmaBufs.pop_back();
            }
            for (auto& existing : m_dmaBufs) existing.retired = false;
            return false;
        }
        AHardwareBuffer_describe(slot.ahb, &desc);
        slot.stride = desc.stride * 4u;
        const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        slot.image = createImage(m_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                 getNativeClientBuffer(slot.ahb), attributes);
        if (slot.image == EGL_NO_IMAGE_KHR) {
            destroyDmaBufSlot(slot);
            while (m_dmaBufs.size() > retainedPoolSize) {
                destroyDmaBufSlot(m_dmaBufs.back());
                m_dmaBufs.pop_back();
            }
            for (auto& existing : m_dmaBufs) existing.retired = false;
            return false;
        }
        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        imageTarget(GL_TEXTURE_2D, slot.image);
        glGenFramebuffers(1, &slot.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, slot.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, slot.texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            destroyDmaBufSlot(slot);
            while (m_dmaBufs.size() > retainedPoolSize) {
                destroyDmaBufSlot(m_dmaBufs.back());
                m_dmaBufs.pop_back();
            }
            for (auto& existing : m_dmaBufs) existing.retired = false;
            return false;
        }
        m_dmaBufs.push_back(slot);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_dmaBufCapacityWidth = width;
    m_dmaBufCapacityHeight = height;
    if (!m_dmaBufTransportLogged) {
        std::cerr << "[LCL Canvas] Client AHardwareBuffer transport active (3 buffers)\n";
        m_dmaBufTransportLogged = true;
    }
    return true;
#else
    if (!m_initialized || !m_gbmDevice || width == 0 || height == 0 || !makeCurrent()) return false;

    const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    const auto imageTarget = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage || !destroyImage || !imageTarget) return false;

    // Three independent BOs prevent the client from writing a frame the
    // compositor still samples. Every slot becomes reusable only via
    // ReleaseDmaBuf.
    // Free slots from the old generation can be destroyed immediately. Busy
    // slots stay alive, retired, until their compositor release arrives.
    for (size_t index = m_dmaBufs.size(); index > 0; --index) {
        auto& slot = m_dmaBufs[index - 1];
        if (slot.busy) {
            slot.retired = true;
        } else {
            destroyDmaBufSlot(slot);
            m_dmaBufs.erase(m_dmaBufs.begin() + static_cast<std::ptrdiff_t>(index - 1));
        }
    }
    const size_t retainedPoolSize = m_dmaBufs.size();
    for (uint32_t index = 0; index < 3; ++index) {
        DmaBufSlot slot{};
        slot.id = m_nextDmaBufId++;
        slot.width = width;
        slot.height = height;
        slot.bo = gbm_bo_create(m_gbmDevice, width, height, GBM_FORMAT_ARGB8888,
                                GBM_BO_USE_RENDERING);
        if (!slot.bo) {
            while (m_dmaBufs.size() > retainedPoolSize) {
                destroyDmaBufSlot(m_dmaBufs.back());
                m_dmaBufs.pop_back();
            }
            for (auto& existing : m_dmaBufs) existing.retired = false;
            return false;
        }
        const EGLint attributes[] = {EGL_NONE};
        slot.image = createImage(m_display, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
                                 reinterpret_cast<EGLClientBuffer>(slot.bo), attributes);
        if (slot.image == EGL_NO_IMAGE_KHR) {
            destroyDmaBufSlot(slot);
            while (m_dmaBufs.size() > retainedPoolSize) {
                destroyDmaBufSlot(m_dmaBufs.back());
                m_dmaBufs.pop_back();
            }
            for (auto& existing : m_dmaBufs) existing.retired = false;
            return false;
        }
        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        imageTarget(GL_TEXTURE_2D, slot.image);
        glGenFramebuffers(1, &slot.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, slot.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               slot.texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            destroyDmaBufSlot(slot);
            while (m_dmaBufs.size() > retainedPoolSize) {
                destroyDmaBufSlot(m_dmaBufs.back());
                m_dmaBufs.pop_back();
            }
            for (auto& existing : m_dmaBufs) existing.retired = false;
            return false;
        }
        slot.stride = gbm_bo_get_stride(slot.bo);
        slot.modifier = gbm_bo_get_modifier(slot.bo);
        m_dmaBufs.push_back(slot);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_dmaBufCapacityWidth = width;
    m_dmaBufCapacityHeight = height;
    if (!m_dmaBufTransportLogged) {
        std::cerr << "[LCL Canvas] Client DMA-BUF transport active (3 GBM buffers)\n";
        m_dmaBufTransportLogged = true;
    }
    return true;
#endif
}

bool ClientEGLContext::ensureDmaBufCapacity(uint32_t width, uint32_t height) {
    if (!m_initialized || width == 0 || height == 0) return false;
    if (!m_dmaBufs.empty() && width <= m_dmaBufCapacityWidth &&
        height <= m_dmaBufCapacityHeight) return true;
    return appendDmaBufPool(std::max(width, m_dmaBufCapacityWidth),
                            std::max(height, m_dmaBufCapacityHeight));
}

void ClientEGLContext::destroyDmaBufSlot(DmaBufSlot& slot) {
    const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    if (slot.framebuffer) glDeleteFramebuffers(1, &slot.framebuffer);
    if (slot.texture) glDeleteTextures(1, &slot.texture);
    if (slot.image != EGL_NO_IMAGE_KHR && destroyImage) destroyImage(m_display, slot.image);
#if defined(__ANDROID__)
    if (slot.ahb) AHardwareBuffer_release(slot.ahb);
#endif
#if !defined(__ANDROID__)
    if (slot.bo) gbm_bo_destroy(slot.bo);
#endif
    if (slot.releaseFenceFd >= 0) close(slot.releaseFenceFd);
    slot = {};
}

void ClientEGLContext::destroyDmaBufPool() {
    if (!m_dmaBufs.empty() && m_display != EGL_NO_DISPLAY) {
        makeCurrent();
        for (auto& slot : m_dmaBufs) {
            destroyDmaBufSlot(slot);
        }
    }
    m_dmaBufs.clear();
    m_currentDmaBuf = -1;
    m_dmaBufCapacityWidth = 0;
    m_dmaBufCapacityHeight = 0;
}

std::optional<ClientEGLContext::DmaBufTarget> ClientEGLContext::acquireDmaBufTarget() {
    if (!m_initialized || m_currentDmaBuf >= 0 || !makeCurrent()) return std::nullopt;
    for (size_t index = 0; index < m_dmaBufs.size(); ++index) {
        auto& slot = m_dmaBufs[index];
        if (slot.busy || slot.retired) continue;
#if defined(__ANDROID__)
        if (slot.releaseFenceFd >= 0) {
            using CreateSync = EGLSyncKHR (*)(EGLDisplay, EGLenum, const EGLint*);
            using DestroySync = EGLBoolean (*)(EGLDisplay, EGLSyncKHR);
            using WaitSync = EGLBoolean (*)(EGLDisplay, EGLSyncKHR, EGLint);
            const auto createSync = reinterpret_cast<CreateSync>(
                eglGetProcAddress("eglCreateSyncKHR"));
            const auto destroySync = reinterpret_cast<DestroySync>(
                eglGetProcAddress("eglDestroySyncKHR"));
            const auto waitSync = reinterpret_cast<WaitSync>(
                eglGetProcAddress("eglWaitSyncKHR"));
            const EGLint attributes[] = {
                EGL_SYNC_NATIVE_FENCE_FD_ANDROID, slot.releaseFenceFd, EGL_NONE};
            EGLSyncKHR sync = createSync
                ? createSync(m_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes)
                : EGL_NO_SYNC_KHR;
            if (sync != EGL_NO_SYNC_KHR && waitSync && destroySync) {
                slot.releaseFenceFd = -1; // EGL owns the imported fd.
                waitSync(m_display, sync, 0);
                destroySync(m_display, sync);
            } else {
                pollfd descriptor{slot.releaseFenceFd, POLLIN, 0};
                (void)poll(&descriptor, 1, 3000);
                close(slot.releaseFenceFd);
                slot.releaseFenceFd = -1;
            }
        }
#endif
        slot.busy = true;
        m_currentDmaBuf = static_cast<int>(index);
        return DmaBufTarget{slot.id, slot.framebuffer, slot.texture, slot.width, slot.height};
    }
    return std::nullopt;
}

std::optional<ClientEGLContext::DmaBufExport> ClientEGLContext::exportCurrentDmaBuf() {
#if defined(__ANDROID__)
    if (m_currentDmaBuf < 0 ||
        static_cast<size_t>(m_currentDmaBuf) >= m_dmaBufs.size())
        return std::nullopt;
    auto& slot = m_dmaBufs[static_cast<size_t>(m_currentDmaBuf)];
    m_currentDmaBuf = -1;
    int acquireFenceFd = -1;
    using CreateSync = EGLSyncKHR (*)(EGLDisplay, EGLenum, const EGLint*);
    using DestroySync = EGLBoolean (*)(EGLDisplay, EGLSyncKHR);
    using DupFence = EGLint (*)(EGLDisplay, EGLSyncKHR);
    const auto createSync = reinterpret_cast<CreateSync>(
        eglGetProcAddress("eglCreateSyncKHR"));
    const auto destroySync = reinterpret_cast<DestroySync>(
        eglGetProcAddress("eglDestroySyncKHR"));
    const auto dupFence = reinterpret_cast<DupFence>(
        eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (createSync && destroySync && dupFence) {
        const EGLint attributes[] = {EGL_NONE};
        EGLSyncKHR sync = createSync(
            m_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
        if (sync != EGL_NO_SYNC_KHR) {
            glFlush();
            acquireFenceFd = dupFence(m_display, sync);
            destroySync(m_display, sync);
        }
    }
    if (acquireFenceFd < 0) glFinish();
    return DmaBufExport{slot.id, slot.width, slot.height, slot.stride,
                        lcl::platform::kDmaBufFormatArgb8888,
                        ~uint64_t{0}, -1, true, acquireFenceFd};
#else
    if (m_currentDmaBuf < 0 || static_cast<size_t>(m_currentDmaBuf) >= m_dmaBufs.size()) return std::nullopt;
    auto& slot = m_dmaBufs[static_cast<size_t>(m_currentDmaBuf)];
    const int fd = gbm_bo_get_fd(slot.bo);
    m_currentDmaBuf = -1;
    if (fd < 0) {
        slot.busy = false;
        return std::nullopt;
    }
    glFlush();
    return DmaBufExport{slot.id, slot.width, slot.height, slot.stride,
                        lcl::platform::kDmaBufFormatArgb8888, slot.modifier,
                        fd, false, -1};
#endif
}

bool ClientEGLContext::sendNativeBufferHandle(int socketFd, uint32_t bufferId) {
#if defined(__ANDROID__)
    if (socketFd < 0) return false;
    for (const auto& slot : m_dmaBufs) {
        if (slot.id == bufferId && slot.ahb) {
            return AHardwareBuffer_sendHandleToUnixSocket(slot.ahb, socketFd) == 0;
        }
    }
#else
    (void)socketFd;
    (void)bufferId;
#endif
    return false;
}

void ClientEGLContext::cancelCurrentDmaBuf() {
    if (m_currentDmaBuf >= 0 && static_cast<size_t>(m_currentDmaBuf) < m_dmaBufs.size()) {
        m_dmaBufs[static_cast<size_t>(m_currentDmaBuf)].busy = false;
    }
    m_currentDmaBuf = -1;
}

void ClientEGLContext::releaseDmaBuf(uint32_t bufferId, int releaseFenceFd) {
    for (size_t index = 0; index < m_dmaBufs.size(); ++index) {
        auto& slot = m_dmaBufs[index];
        if (slot.id != bufferId) continue;
        if (slot.releaseFenceFd >= 0) close(slot.releaseFenceFd);
        slot.releaseFenceFd = releaseFenceFd;
        slot.busy = false;
        if (slot.retired) {
            if (makeCurrent()) destroyDmaBufSlot(slot);
            m_dmaBufs.erase(m_dmaBufs.begin() + static_cast<std::ptrdiff_t>(index));
        }
        return;
    }
    if (releaseFenceFd >= 0) close(releaseFenceFd);
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
    destroyDmaBufPool();
    if (m_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface != EGL_NO_SURFACE) eglDestroySurface(m_display, m_surface);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }
    m_surface = EGL_NO_SURFACE;
    m_context = EGL_NO_CONTEXT;
    m_display = EGL_NO_DISPLAY;
#if !defined(__ANDROID__)
    if (m_gbmDevice) gbm_device_destroy(m_gbmDevice);
#endif
    m_gbmDevice = nullptr;
    if (m_renderFd >= 0) close(m_renderFd);
    m_renderFd = -1;
    m_width = 0;
    m_height = 0;
    m_initialized = false;
    m_hardwareAccelerated = false;
}

} // namespace lcl::render
