#include "platform/desktop/gbm_graphics_context.hpp"

#include <iostream>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <algorithm>
#include <vector>
#include <utility>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

namespace lcl::platform::desktop {

GbmGraphicsContext::~GbmGraphicsContext() {
    shutdown();
}

GbmGraphicsContext::GbmGraphicsContext(GbmGraphicsContext&& other) noexcept
    : m_drmFd(other.m_drmFd),
      m_width(other.m_width),
      m_height(other.m_height),
      m_crtcId(other.m_crtcId),
      m_connectorId(other.m_connectorId),
      m_gbmDevice(other.m_gbmDevice),
      m_gbmSurface(other.m_gbmSurface),
      m_previousBO(other.m_previousBO),
      m_eglDisplay(other.m_eglDisplay),
      m_eglContext(other.m_eglContext),
      m_eglSurface(other.m_eglSurface),
      m_eglConfig(other.m_eglConfig),
      m_currentFBId(other.m_currentFBId),
      m_crtcSet(other.m_crtcSet),
      m_vsyncActive(other.m_vsyncActive),
      m_initialized(other.m_initialized),
      m_glRendererString(std::move(other.m_glRendererString)),
      m_isHardwareAccelerated(other.m_isHardwareAccelerated),
      m_importedDmaBufImages(std::move(other.m_importedDmaBufImages)) {
    other.m_drmFd = -1;
    other.m_gbmDevice = nullptr;
    other.m_gbmSurface = nullptr;
    other.m_previousBO = nullptr;
    other.m_eglDisplay = EGL_NO_DISPLAY;
    other.m_eglContext = EGL_NO_CONTEXT;
    other.m_eglSurface = EGL_NO_SURFACE;
    other.m_initialized = false;
    other.m_importedDmaBufImages.clear();
}

GbmGraphicsContext& GbmGraphicsContext::operator=(GbmGraphicsContext&& other) noexcept {
    if (this != &other) {
        shutdown();

        m_drmFd = other.m_drmFd;
        m_width = other.m_width;
        m_height = other.m_height;
        m_crtcId = other.m_crtcId;
        m_connectorId = other.m_connectorId;
        m_gbmDevice = other.m_gbmDevice;
        m_gbmSurface = other.m_gbmSurface;
        m_previousBO = other.m_previousBO;
        m_eglDisplay = other.m_eglDisplay;
        m_eglContext = other.m_eglContext;
        m_eglSurface = other.m_eglSurface;
        m_eglConfig = other.m_eglConfig;
        m_currentFBId = other.m_currentFBId;
        m_crtcSet = other.m_crtcSet;
        m_vsyncActive = other.m_vsyncActive;
        m_initialized = other.m_initialized;
        m_glRendererString = std::move(other.m_glRendererString);
        m_isHardwareAccelerated = other.m_isHardwareAccelerated;
        m_importedDmaBufImages = std::move(other.m_importedDmaBufImages);

        other.m_drmFd = -1;
        other.m_gbmDevice = nullptr;
        other.m_gbmSurface = nullptr;
        other.m_previousBO = nullptr;
        other.m_eglDisplay = EGL_NO_DISPLAY;
        other.m_eglContext = EGL_NO_CONTEXT;
        other.m_eglSurface = EGL_NO_SURFACE;
        other.m_initialized = false;
        other.m_importedDmaBufImages.clear();
    }
    return *this;
}

bool GbmGraphicsContext::initialize(int drmFd, uint32_t width, uint32_t height, uint32_t crtcId, uint32_t connectorId) {
    if (m_initialized) return true;

    if (drmFd < 0 || width == 0 || height == 0) {
        std::cerr << "[LCL EGL] Invalid parameters for EGL initialization.\n";
        return false;
    }

    m_drmFd = drmFd;
    m_width = width;
    m_height = height;
    m_crtcId = crtcId;
    m_connectorId = connectorId;

    // Set driver search paths for Mesa in isolated initramfs
    setenv("LIBGL_DRIVERS_PATH", "/usr/lib/x86_64-linux-gnu/dri:/usr/lib/dri", 1);
    setenv("GBM_DRIVERS_PATH", "/usr/lib/x86_64-linux-gnu/gbm:/usr/lib/gbm", 1);

    // 1. Create GBM Device
    m_gbmDevice = gbm_create_device(m_drmFd);
    if (!m_gbmDevice) {
        std::cerr << "[LCL EGL] Failed to create GBM device on DRM fd: " << m_drmFd << "\n";
        return false;
    }
    std::cout << "[LCL EGL] GBM device initialized successfully on DRM card.\n";

    // 2. Create GBM Surface for Scanout + Rendering
    m_gbmSurface = gbm_surface_create(m_gbmDevice, m_width, m_height,
                                      GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!m_gbmSurface) {
        std::cerr << "[LCL EGL] Failed to create GBM surface (" << m_width << "x" << m_height << ").\n";
        shutdown();
        return false;
    }

    // 3. Bind API prior to display initialization
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        eglBindAPI(EGL_OPENGL_API);
    }

    // 4. Initialize EGL Display with candidates
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplayExt =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLGETPLATFORMDISPLAYPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYPROC)eglGetProcAddress("eglGetPlatformDisplay");

    static const EGLint emptyAttribsExt[] = { EGL_NONE };
    static const EGLAttrib emptyAttribs[] = { EGL_NONE };

    EGLint major = 0, minor = 0;
    auto tryInitializeDisplay = [&](EGLDisplay dpy, const char* name) -> bool {
        if (dpy == EGL_NO_DISPLAY) {
            EGLint err = eglGetError();
            std::cout << "[LCL EGL] Probe '" << name << "' returned EGL_NO_DISPLAY (eglGetError: 0x"
                      << std::hex << err << std::dec << ")\n";
            return false;
        }
        if (eglInitialize(dpy, &major, &minor)) {
            m_eglDisplay = dpy;
            std::cout << "[LCL EGL] EGL display successfully acquired via '" << name
                      << "' (v" << major << "." << minor
                      << ", Vendor: " << (eglQueryString(m_eglDisplay, EGL_VENDOR) ? eglQueryString(m_eglDisplay, EGL_VENDOR) : "Unknown") << ")\n";
            return true;
        }
        EGLint err = eglGetError();
        std::cout << "[LCL EGL] eglInitialize failed for '" << name << "' (eglGetError: 0x"
                  << std::hex << err << std::dec << ")\n";
        return false;
    };

    bool ok = false;
    // Candidate 1: Standard eglGetDisplay with gbmDevice (canonical Mesa GBM display)
    if (!ok && m_gbmDevice) {
        ok = tryInitializeDisplay(eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(m_gbmDevice)), "eglGetDisplay(gbmDevice)");
    }
    // Candidate 2: eglGetPlatformDisplayEXT with EGL_PLATFORM_GBM_KHR
    if (!ok && getPlatformDisplayExt && m_gbmDevice) {
        ok = tryInitializeDisplay(getPlatformDisplayExt(EGL_PLATFORM_GBM_KHR, m_gbmDevice, emptyAttribsExt), "eglGetPlatformDisplayEXT(GBM_KHR)");
    }
    // Candidate 3: eglGetPlatformDisplay with EGL_PLATFORM_GBM_KHR
    if (!ok && getPlatformDisplay && m_gbmDevice) {
        ok = tryInitializeDisplay(getPlatformDisplay(EGL_PLATFORM_GBM_KHR, m_gbmDevice, emptyAttribs), "eglGetPlatformDisplay(GBM_KHR)");
    }
    // Candidate 4: eglGetDisplay with DRM file descriptor
    if (!ok && m_drmFd >= 0) {
        ok = tryInitializeDisplay(eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(static_cast<uintptr_t>(m_drmFd))), "eglGetDisplay(drmFd)");
    }
    // Candidate 5: eglGetDisplay with EGL_DEFAULT_DISPLAY
    if (!ok) {
        ok = tryInitializeDisplay(eglGetDisplay(EGL_DEFAULT_DISPLAY), "eglGetDisplay(DEFAULT)");
    }

    if (!ok || m_eglDisplay == EGL_NO_DISPLAY) {
        std::cerr << "[LCL EGL ERROR] Failed to acquire any valid EGL display.\n";
        shutdown();
        return false;
    }

    // 5. Choose EGL Config with progressive fallbacks
    static const EGLint config1[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    static const EGLint config2[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    static const EGLint config3[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    static const EGLint config4[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    static const EGLint config5[] = {
        EGL_NONE
    };

    const EGLint* fallbacks[] = { config1, config2, config3, config4, config5 };
    EGLint numConfigs = 0;
    bool foundConfig = false;

    for (const auto* attribs : fallbacks) {
        if (eglChooseConfig(m_eglDisplay, attribs, &m_eglConfig, 1, &numConfigs) && numConfigs > 0) {
            foundConfig = true;
            break;
        }
    }

    if (!foundConfig) {
        EGLint err = eglGetError();
        std::cerr << "[LCL EGL ERROR] Failed to find compatible EGL config. eglGetError: 0x"
                  << std::hex << err << std::dec << "\n";
        shutdown();
        return false;
    }

    // 5. Create EGL Context (GLES 3 preferred for EGL 1.5 fence sync, GLES 2 fallback)
    static const EGLint contextAttribs3[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    static const EGLint contextAttribs2[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    m_eglContext = eglCreateContext(m_eglDisplay, m_eglConfig, EGL_NO_CONTEXT, contextAttribs3);
    if (m_eglContext == EGL_NO_CONTEXT) {
        m_eglContext = eglCreateContext(m_eglDisplay, m_eglConfig, EGL_NO_CONTEXT, contextAttribs2);
    }
    if (m_eglContext == EGL_NO_CONTEXT) {
        std::cerr << "[LCL EGL] Failed to create EGL context: 0x" << std::hex << eglGetError() << std::dec << "\n";
        shutdown();
        return false;
    }

    // 6. Create EGL Window Surface from GBM surface
    m_eglSurface = eglCreateWindowSurface(m_eglDisplay, m_eglConfig, (EGLNativeWindowType)m_gbmSurface, NULL);
    if (m_eglSurface == EGL_NO_SURFACE) {
        std::cerr << "[LCL EGL] Failed to create EGL window surface: 0x" << std::hex << eglGetError() << std::dec << "\n";
        shutdown();
        return false;
    }

    if (!makeCurrent()) {
        std::cerr << "[LCL EGL] Failed to make EGL context current.\n";
        shutdown();
        return false;
    }

    // Enable hardware VSync synchronization (1 = wait for VBlank on buffer swap)
    m_vsyncActive = false;
    if (eglSwapInterval(m_eglDisplay, 1) == EGL_TRUE) {
        m_vsyncActive = true;
        std::cout << "[LCL EGL] Hardware VSync enabled (eglSwapInterval = 1).\n";
    } else {
        std::cerr << "[LCL EGL WARNING] Failed to set eglSwapInterval(1).\n";
    }

    // GL Renderer Audit: Detect virgl vs llvmpipe/software rasterizer
    const char* glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* glVendor   = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* glVersion  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::string rendererStr  = glRenderer  ? glRenderer  : "";
    std::string vendorStr    = glVendor    ? glVendor    : "";
    std::string versionStr   = glVersion   ? glVersion   : "";

    auto containsCI = [](const std::string& s, const char* key) {
        std::string l = s; std::string k = key;
        std::transform(l.begin(), l.end(), l.begin(), ::tolower);
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        return l.find(k) != std::string::npos;
    };

    m_isHardwareAccelerated = containsCI(rendererStr, "virgl") || containsCI(rendererStr, "virtio")
                           || containsCI(vendorStr,   "virgl") || containsCI(vendorStr,   "virtio");
    bool isSW = containsCI(rendererStr, "llvmpipe") || containsCI(rendererStr, "softpipe")
             || containsCI(rendererStr, "swrast")   || containsCI(rendererStr, "software");

    if (m_isHardwareAccelerated) {
        m_glRendererString = "VirGL 3D (GPU)";
        std::cout << "[LCL Display] Render Engine: HARDWARE ACCELERATED (VirGL 3D - " << rendererStr << ")\n";
    } else if (isSW) {
        m_glRendererString = "Mesa llvmpipe (CPU)";
        std::cout << "[LCL Display] Render Engine: SOFTWARE EMULATED (Mesa llvmpipe CPU - " << rendererStr << ")\n";
    } else {
        m_glRendererString = rendererStr.empty() ? "Software Fallback" : rendererStr;
        std::cout << "[LCL Display] Render Engine: " << m_glRendererString
                  << " (Vendor: " << (vendorStr.empty() ? "Unknown" : vendorStr) << ")\n";
    }
    std::cout << "[LCL Display] GL Version: " << (versionStr.empty() ? "Unknown" : versionStr) << "\n";

    m_initialized = true;
    std::cout << "[LCL EGL] Hardware-Accelerated EGL/GBM Context active (" << m_width << "x" << m_height << ")!\n";
    return true;
}

bool GbmGraphicsContext::makeCurrent() {
    if (m_eglDisplay == EGL_NO_DISPLAY || m_eglContext == EGL_NO_CONTEXT) return false;
    return eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext) == EGL_TRUE;
}

bool GbmGraphicsContext::resize(uint32_t width, uint32_t height) {
    return width == m_width && height == m_height;
}

uint32_t GbmGraphicsContext::getOrCreateFB(struct gbm_bo* bo) {
    if (!bo) return 0;
    uint32_t fbId = reinterpret_cast<uintptr_t>(gbm_bo_get_user_data(bo));
    if (fbId != 0) return fbId;

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;

    int ret = drmModeAddFB(m_drmFd, width, height, 24, 32, stride, handle, &fbId);
    if (ret != 0) {
        int primeFd = gbm_bo_get_fd(bo);
        if (primeFd >= 0) {
            uint32_t primeHandle = 0;
            if (drmPrimeFDToHandle(m_drmFd, primeFd, &primeHandle) == 0) {
                ret = drmModeAddFB(m_drmFd, width, height, 24, 32, stride, primeHandle, &fbId);
            }
            close(primeFd);
        }
    }

    if (ret != 0) {
        static bool s_logged = false;
        if (!s_logged) {
            std::cerr << "[LCL EGL WARNING] drmModeAddFB failed for GBM buffer: " << std::strerror(errno) << "\n";
            s_logged = true;
        }
        return 0;
    }

    gbm_bo_set_user_data(bo, reinterpret_cast<void*>(static_cast<uintptr_t>(fbId)), [](struct gbm_bo* bo, void* data) {
        uint32_t fbId = reinterpret_cast<uintptr_t>(data);
        if (fbId != 0) {
            int fd = gbm_device_get_fd(gbm_bo_get_device(bo));
            drmModeRmFB(fd, fbId);
        }
    });

    return fbId;
}

bool GbmGraphicsContext::swapBuffers() {
    if (!m_initialized) return false;

    // Swap EGL front/back buffers
    if (!eglSwapBuffers(m_eglDisplay, m_eglSurface)) {
        std::cerr << "[LCL EGL] eglSwapBuffers failed: 0x" << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }

    // Lock front BO from GBM surface
    struct gbm_bo* bo = gbm_surface_lock_front_buffer(m_gbmSurface);
    if (!bo) {
        return false;
    }

    uint32_t fbId = getOrCreateFB(bo);
    m_currentFBId = fbId;

    // Perform DRM Page Flip / Scanout update if CRTC ID is set
    if (m_crtcId > 0 && fbId > 0) {
        int ret = drmModePageFlip(m_drmFd, m_crtcId, fbId, 0, nullptr);
        if (ret != 0 && !m_crtcSet) {
            drmModeCrtcPtr crtc = drmModeGetCrtc(m_drmFd, m_crtcId);
            if (crtc) {
                if (drmModeSetCrtc(m_drmFd, m_crtcId, fbId, 0, 0, &m_connectorId, 1, &crtc->mode) == 0) {
                    m_crtcSet = true;
                    std::cout << "[LCL EGL] Initial DRM CRTC modeset configured successfully (FB ID: " << fbId << ").\n";
                }
                drmModeFreeCrtc(crtc);
            }
        } else {
            m_crtcSet = true;
        }
    }

    // Release previously rendered buffer back to GBM surface pool
    if (m_previousBO) {
        gbm_surface_release_buffer(m_gbmSurface, m_previousBO);
    }
    m_previousBO = bo;

    return true;
}

lcl::platform::TextureHandle GbmGraphicsContext::importTexture(const lcl::platform::INativeBuffer& buffer) {
    const auto* dmaBuf = dynamic_cast<const DmaBufNativeBuffer*>(&buffer);
    if (!dmaBuf) {
        return lcl::platform::kInvalidTextureHandle;
    }

    int fd = dmaBuf->fd();
    uint32_t width = dmaBuf->width();
    uint32_t height = dmaBuf->height();
    uint32_t stride = dmaBuf->stride();
    uint32_t format = dmaBuf->format();
    uint64_t modifier = dmaBuf->modifier();

    if (!m_initialized || fd < 0 || width == 0 || height == 0 ||
        stride < width * 4 || format != 1 || !makeCurrent()) {
        return lcl::platform::kInvalidTextureHandle;
    }

    const char* extensions = eglQueryString(m_eglDisplay, EGL_EXTENSIONS);
    if (!extensions || !std::strstr(extensions, "EGL_EXT_image_dma_buf_import")) {
        return lcl::platform::kInvalidTextureHandle;
    }

    const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    const auto imageTarget = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage || !imageTarget) return lcl::platform::kInvalidTextureHandle;

    const uint32_t fourcc = GBM_FORMAT_ARGB8888;
    EGLint attributes[32] = {
        EGL_WIDTH, static_cast<EGLint>(width),
        EGL_HEIGHT, static_cast<EGLint>(height),
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride),
        EGL_NONE,
    };
    size_t end = 12;
    if (modifier != ~uint64_t{0}) {
        attributes[end++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attributes[end++] = static_cast<EGLint>(modifier & 0xFFFFFFFFu);
        attributes[end++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attributes[end++] = static_cast<EGLint>(modifier >> 32u);
        attributes[end++] = EGL_NONE;
    }
    const EGLImageKHR image = createImage(m_eglDisplay, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    if (image == EGL_NO_IMAGE_KHR) return lcl::platform::kInvalidTextureHandle;

    uint32_t texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    imageTarget(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
        if (destroyImage) destroyImage(m_eglDisplay, image);
        if (texture) glDeleteTextures(1, &texture);
        return lcl::platform::kInvalidTextureHandle;
    }
    m_importedDmaBufImages.emplace(texture, image);
    return texture;
}

lcl::platform::TextureHandle GbmGraphicsContext::importDmaBuf(const lcl::platform::DmaBufDescriptor& descriptor) {
    DmaBufNativeBuffer buffer(descriptor.fd, descriptor.width, descriptor.height,
                              descriptor.stride, descriptor.format, descriptor.modifier);
    return importTexture(buffer);
}

void GbmGraphicsContext::releaseTexture(lcl::platform::TextureHandle texture) {
    const auto found = m_importedDmaBufImages.find(texture);
    if (found == m_importedDmaBufImages.end()) return;
    if (m_initialized && makeCurrent()) {
        glDeleteTextures(1, &texture);
        const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
        if (destroyImage) destroyImage(m_eglDisplay, found->second);
    }
    m_importedDmaBufImages.erase(found);
}

void GbmGraphicsContext::shutdown() {
    if (m_initialized && makeCurrent()) {
        std::vector<uint32_t> textures;
        textures.reserve(m_importedDmaBufImages.size());
        for (const auto& [texture, _] : m_importedDmaBufImages) textures.push_back(texture);
        for (uint32_t texture : textures) releaseTexture(texture);
    }
    if (m_eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(m_eglDisplay, m_eglSurface);
            m_eglSurface = EGL_NO_SURFACE;
        }
        if (m_eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(m_eglDisplay, m_eglContext);
            m_eglContext = EGL_NO_CONTEXT;
        }
        eglTerminate(m_eglDisplay);
        m_eglDisplay = EGL_NO_DISPLAY;
    }

    if (m_gbmSurface) {
        if (m_previousBO) {
            gbm_surface_release_buffer(m_gbmSurface, m_previousBO);
            m_previousBO = nullptr;
        }
        gbm_surface_destroy(m_gbmSurface);
        m_gbmSurface = nullptr;
    }

    if (m_gbmDevice) {
        gbm_device_destroy(m_gbmDevice);
        m_gbmDevice = nullptr;
    }

    m_crtcSet = false;
    m_initialized = false;
}

} // namespace lcl::platform::desktop
