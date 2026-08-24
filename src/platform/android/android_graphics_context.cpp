#include "platform/android/android_graphics_context.hpp"
#include "platform/android/android_display_backend.hpp"

#include <iostream>
#include <vector>
#include <cstring>
#include <cassert>
#include <unistd.h>

typedef EGLClientBuffer (*pfn_eglGetNativeClientBufferANDROID)(const struct AHardwareBuffer* buffer);
typedef EGLImageKHR (*pfn_eglCreateImageKHR)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint* attrib_list);
typedef EGLBoolean (*pfn_eglDestroyImageKHR)(EGLDisplay dpy, EGLImageKHR image);
typedef void (*pfn_glEGLImageTargetTexture2DOES)(GLenum target, void* image);
typedef EGLSyncKHR (*pfn_eglCreateSyncKHR)(EGLDisplay, EGLenum, const EGLint*);
typedef EGLBoolean (*pfn_eglDestroySyncKHR)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*pfn_eglDupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
typedef EGLBoolean (*pfn_eglWaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint);

namespace lcl::platform::android {

AndroidGraphicsContext::AndroidGraphicsContext() = default;

AndroidGraphicsContext::~AndroidGraphicsContext() {
    shutdown();
}

bool AndroidGraphicsContext::setupScanoutBuffers() {
    auto eglGetNativeClientBufferANDROID = reinterpret_cast<pfn_eglGetNativeClientBufferANDROID>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    auto eglCreateImageKHR = reinterpret_cast<pfn_eglCreateImageKHR>(eglGetProcAddress("eglCreateImageKHR"));
    auto glEGLImageTargetTexture2DOES = reinterpret_cast<pfn_glEGLImageTargetTexture2DOES>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (!eglGetNativeClientBufferANDROID || !eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
        std::cerr << "[AndroidGraphicsContext] Missing required EGLImage/AHB extension entry points\n";
        return false;
    }

    for (size_t i = 0; i < m_scanoutSlots.size(); ++i) {
        AHardwareBuffer_Desc desc = {};
        desc.width = m_width;
        desc.height = m_height;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

        int allocRes = AHardwareBuffer_allocate(&desc, &m_scanoutSlots[i].ahb);
        if (allocRes != 0 || !m_scanoutSlots[i].ahb) {
            std::cerr << "[AndroidGraphicsContext] Failed to allocate scanout AHardwareBuffer slot " << i << "\n";
            return false;
        }

        EGLClientBuffer clientBuf = eglGetNativeClientBufferANDROID(m_scanoutSlots[i].ahb);
        EGLint imgAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        m_scanoutSlots[i].eglImage = eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuf, imgAttrs);
        if (m_scanoutSlots[i].eglImage == EGL_NO_IMAGE_KHR) {
            std::cerr << "[AndroidGraphicsContext] Failed to create EGLImage for scanout slot " << i << "\n";
            return false;
        }

        glGenTextures(1, &m_scanoutSlots[i].texture);
        glBindTexture(GL_TEXTURE_2D, m_scanoutSlots[i].texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_scanoutSlots[i].eglImage);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &m_scanoutSlots[i].fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_scanoutSlots[i].fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scanoutSlots[i].texture, 0);

        GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[AndroidGraphicsContext] Framebuffer incomplete for scanout slot " << i << " (status: " << fboStatus << ")\n";
            return false;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void AndroidGraphicsContext::destroyScanoutBuffers() {
    auto eglDestroyImageKHR = reinterpret_cast<pfn_eglDestroyImageKHR>(eglGetProcAddress("eglDestroyImageKHR"));

    for (size_t i = 0; i < m_scanoutSlots.size(); ++i) {
        if (m_scanoutSlots[i].fbo != 0) {
            glDeleteFramebuffers(1, &m_scanoutSlots[i].fbo);
            m_scanoutSlots[i].fbo = 0;
        }
        if (m_scanoutSlots[i].texture != 0) {
            glDeleteTextures(1, &m_scanoutSlots[i].texture);
            m_scanoutSlots[i].texture = 0;
        }
        if (m_scanoutSlots[i].eglImage != EGL_NO_IMAGE_KHR && eglDestroyImageKHR) {
            eglDestroyImageKHR(m_eglDisplay, m_scanoutSlots[i].eglImage);
            m_scanoutSlots[i].eglImage = EGL_NO_IMAGE_KHR;
        }
        if (m_scanoutSlots[i].ahb != nullptr) {
            AHardwareBuffer_release(m_scanoutSlots[i].ahb);
            m_scanoutSlots[i].ahb = nullptr;
        }
    }
}

bool AndroidGraphicsContext::initialize(uint32_t width, uint32_t height,
                                        AndroidDisplayBackend* displayBackend) {
    if (m_initialized) return true;

    m_width = width > 0 ? width : 320;
    m_height = height > 0 ? height : 640;
    m_displayBackend = displayBackend;

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

    // 3. Create PBuffer surface (as default EGL surface)
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

    // 7. Setup scanout buffers
    if (!setupScanoutBuffers()) {
        std::cerr << "[AndroidGraphicsContext] Failed to setup scanout buffers\n";
        return false;
    }

    m_initialized = true;
    return true;
}

void AndroidGraphicsContext::shutdown() {
    if (!m_initialized) return;

    destroyScanoutBuffers();

    auto eglDestroyImageKHR = reinterpret_cast<pfn_eglDestroyImageKHR>(eglGetProcAddress("eglDestroyImageKHR"));
    for (auto& [tex, img] : m_importedImages) {
        if (tex != 0) glDeleteTextures(1, &tex);
        if (img != EGL_NO_IMAGE_KHR && eglDestroyImageKHR) {
            eglDestroyImageKHR(m_eglDisplay, img);
        }
    }
    m_importedImages.clear();

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

    destroyScanoutBuffers();

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
    makeCurrent();

    return setupScanoutBuffers();
}

bool AndroidGraphicsContext::present() {
    return presentFromFramebuffer(0, m_width, m_height);
}

bool AndroidGraphicsContext::presentFramebuffer(uint32_t framebuffer,
                                                uint32_t width,
                                                uint32_t height) {
    if (framebuffer == 0) return false;
    return presentFromFramebuffer(framebuffer, width, height);
}

bool AndroidGraphicsContext::presentFromFramebuffer(uint32_t framebuffer,
                                                    uint32_t width,
                                                    uint32_t height) {
    if (!m_initialized || width == 0 || height == 0 ||
        width > m_width || height > m_height) return false;

    auto& scanout = m_scanoutSlots[m_currentSlotIndex];
    if (!m_displayBackend || !scanout.ahb ||
        !m_displayBackend->prepareBufferForRender(scanout.ahb)) {
        return false;
    }

    // 1. Copy the compositor scene FBO directly into the active scanout AHB.
    // The legacy present() path supplies framebuffer 0 (the PBuffer); modern
    // compositor rendering supplies its retained scene FBO and skips the old
    // scene-texture -> PBuffer full-screen pass.
    // AHardwareBuffer / Android Composer scanout memory origin is top-left (Row 0 is top scanline of display).
    // Blit with inverted destination Y to map UI top to display top scanline.
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scanout.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glBlitFramebuffer(0, 0, width, height,
                      0, height, width, 0,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // Export the GPU completion point to Hardware Composer instead of
    // stalling the CPU on glFinish(). HWC waits asynchronously before reading
    // this AHardwareBuffer. Older vendor EGL stacks retain the safe fallback.
    int acquireFenceFd = -1;
    auto eglCreateSyncKHR = reinterpret_cast<pfn_eglCreateSyncKHR>(
        eglGetProcAddress("eglCreateSyncKHR"));
    auto eglDestroySyncKHR = reinterpret_cast<pfn_eglDestroySyncKHR>(
        eglGetProcAddress("eglDestroySyncKHR"));
    auto eglDupNativeFenceFDANDROID =
        reinterpret_cast<pfn_eglDupNativeFenceFDANDROID>(
            eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (eglCreateSyncKHR && eglDestroySyncKHR && eglDupNativeFenceFDANDROID) {
        const EGLint fenceAttribs[] = {EGL_NONE};
        EGLSyncKHR fence = eglCreateSyncKHR(
            m_eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, fenceAttribs);
        if (fence != EGL_NO_SYNC_KHR) {
            glFlush();
            acquireFenceFd = eglDupNativeFenceFDANDROID(m_eglDisplay, fence);
            eglDestroySyncKHR(m_eglDisplay, fence);
        }
    }
    if (acquireFenceFd < 0) glFinish();

    // 2. Present active AHardwareBuffer through the selected Android Composer backend.
    const bool presented = m_displayBackend->presentBuffer(
        scanout.ahb, acquireFenceFd);
    if (acquireFenceFd >= 0) close(acquireFenceFd);

    // 3. Flip to next buffer slot.
    if (presented) {
        m_currentSlotIndex = (m_currentSlotIndex + 1) % m_scanoutSlots.size();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return presented;
}

bool AndroidGraphicsContext::readback(uint32_t* destination, uint32_t width, uint32_t height) {
    if (!m_initialized || !destination) return false;
    makeCurrent();
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, destination);
    return true;
}

lcl::platform::TextureHandle AndroidGraphicsContext::importTexture(const lcl::platform::INativeBuffer& buffer) {
    const auto* ahbBuffer = dynamic_cast<const AHardwareNativeBuffer*>(&buffer);
    if (!ahbBuffer || !ahbBuffer->getHandle()) return lcl::platform::kInvalidTextureHandle;

    auto eglGetNativeClientBufferANDROID = reinterpret_cast<pfn_eglGetNativeClientBufferANDROID>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    auto eglCreateImageKHR = reinterpret_cast<pfn_eglCreateImageKHR>(eglGetProcAddress("eglCreateImageKHR"));
    auto glEGLImageTargetTexture2DOES = reinterpret_cast<pfn_glEGLImageTargetTexture2DOES>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (!eglGetNativeClientBufferANDROID || !eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
        return lcl::platform::kInvalidTextureHandle;
    }

    EGLClientBuffer clientBuf = eglGetNativeClientBufferANDROID(ahbBuffer->getHandle());
    EGLint imgAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuf, imgAttrs);
    if (img == EGL_NO_IMAGE_KHR) return lcl::platform::kInvalidTextureHandle;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_importedImages[tex] = img;
    return static_cast<lcl::platform::TextureHandle>(tex);
}

void AndroidGraphicsContext::releaseTexture(lcl::platform::TextureHandle texture) {
    if (texture == lcl::platform::kInvalidTextureHandle) return;

    GLuint tex = static_cast<GLuint>(texture);
    auto it = m_importedImages.find(tex);
    if (it != m_importedImages.end()) {
        auto eglDestroyImageKHR = reinterpret_cast<pfn_eglDestroyImageKHR>(eglGetProcAddress("eglDestroyImageKHR"));
        if (it->second != EGL_NO_IMAGE_KHR && eglDestroyImageKHR) {
            eglDestroyImageKHR(m_eglDisplay, it->second);
        }
        m_importedImages.erase(it);
    }
    glDeleteTextures(1, &tex);
}

bool AndroidGraphicsContext::waitNativeFence(int fenceFd) {
    if (fenceFd < 0 || !makeCurrent()) return false;
    auto createSync = reinterpret_cast<pfn_eglCreateSyncKHR>(
        eglGetProcAddress("eglCreateSyncKHR"));
    auto destroySync = reinterpret_cast<pfn_eglDestroySyncKHR>(
        eglGetProcAddress("eglDestroySyncKHR"));
    auto waitSync = reinterpret_cast<pfn_eglWaitSyncKHR>(
        eglGetProcAddress("eglWaitSyncKHR"));
    if (!createSync || !destroySync || !waitSync) return false;
    const EGLint attributes[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd, EGL_NONE};
    EGLSyncKHR sync = createSync(
        m_eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (sync == EGL_NO_SYNC_KHR) return false;
    // EGL owns fenceFd after a successful native-fence import.
    const bool waited = waitSync(m_eglDisplay, sync, 0) == EGL_TRUE;
    destroySync(m_eglDisplay, sync);
    return waited;
}

int AndroidGraphicsContext::createNativeFence() {
    if (!makeCurrent()) return -1;
    auto createSync = reinterpret_cast<pfn_eglCreateSyncKHR>(
        eglGetProcAddress("eglCreateSyncKHR"));
    auto destroySync = reinterpret_cast<pfn_eglDestroySyncKHR>(
        eglGetProcAddress("eglDestroySyncKHR"));
    auto dupFence = reinterpret_cast<pfn_eglDupNativeFenceFDANDROID>(
        eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (!createSync || !destroySync || !dupFence) {
        glFinish();
        return -1;
    }
    const EGLint attributes[] = {EGL_NONE};
    EGLSyncKHR sync = createSync(
        m_eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (sync == EGL_NO_SYNC_KHR) {
        glFinish();
        return -1;
    }
    glFlush();
    const int fenceFd = dupFence(m_eglDisplay, sync);
    destroySync(m_eglDisplay, sync);
    if (fenceFd < 0) glFinish();
    return fenceFd;
}

} // namespace lcl::platform::android
