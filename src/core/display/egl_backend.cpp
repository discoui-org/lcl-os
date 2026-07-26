#include "core/display/egl_backend.hpp"
#include <iostream>
#include <cstring>
#include <cerrno>
#include <unistd.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

namespace lcl::core {

EGLBackend::~EGLBackend() {
    shutdown();
}

EGLBackend::EGLBackend(EGLBackend&& other) noexcept
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
      m_initialized(other.m_initialized) {
    other.m_drmFd = -1;
    other.m_gbmDevice = nullptr;
    other.m_gbmSurface = nullptr;
    other.m_previousBO = nullptr;
    other.m_eglDisplay = EGL_NO_DISPLAY;
    other.m_eglContext = EGL_NO_CONTEXT;
    other.m_eglSurface = EGL_NO_SURFACE;
    other.m_initialized = false;
}

EGLBackend& EGLBackend::operator=(EGLBackend&& other) noexcept {
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
        m_initialized = other.m_initialized;

        other.m_drmFd = -1;
        other.m_gbmDevice = nullptr;
        other.m_gbmSurface = nullptr;
        other.m_previousBO = nullptr;
        other.m_eglDisplay = EGL_NO_DISPLAY;
        other.m_eglContext = EGL_NO_CONTEXT;
        other.m_eglSurface = EGL_NO_SURFACE;
        other.m_initialized = false;
    }
    return *this;
}

bool EGLBackend::initialize(int drmFd, uint32_t width, uint32_t height, uint32_t crtcId, uint32_t connectorId) {
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

    // 3. Initialize EGL Display via eglGetPlatformDisplay / eglGetDisplay
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (getPlatformDisplay) {
        m_eglDisplay = getPlatformDisplay(EGL_PLATFORM_GBM_KHR, m_gbmDevice, NULL);
    }
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        m_eglDisplay = eglGetDisplay((EGLNativeDisplayType)m_gbmDevice);
    }

    if (m_eglDisplay == EGL_NO_DISPLAY) {
        std::cerr << "[LCL EGL] Failed to acquire EGL display from GBM device.\n";
        shutdown();
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_eglDisplay, &major, &minor)) {
        std::cerr << "[LCL EGL] eglInitialize failed: 0x" << std::hex << eglGetError() << std::dec << "\n";
        shutdown();
        return false;
    }
    std::cout << "[LCL EGL] EGL initialized (v" << major << "." << minor
              << ", Vendor: " << eglQueryString(m_eglDisplay, EGL_VENDOR) << ").\n";

    // Bind OpenGL ES API
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        eglBindAPI(EGL_OPENGL_API);
    }

    // 4. Choose EGL Config
    static const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    if (!eglChooseConfig(m_eglDisplay, configAttribs, &m_eglConfig, 1, &numConfigs) || numConfigs == 0) {
        std::cerr << "[LCL EGL] Failed to find compatible EGL config.\n";
        shutdown();
        return false;
    }

    // 5. Create EGL Context (GLES 2/3 / OpenGL)
    static const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    m_eglContext = eglCreateContext(m_eglDisplay, m_eglConfig, EGL_NO_CONTEXT, contextAttribs);
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

    m_initialized = true;
    std::cout << "[LCL EGL] Hardware-Accelerated EGL/GBM Context active (" << m_width << "x" << m_height << ")!\n";
    return true;
}

bool EGLBackend::makeCurrent() {
    if (m_eglDisplay == EGL_NO_DISPLAY || m_eglContext == EGL_NO_CONTEXT) return false;
    return eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext) == EGL_TRUE;
}

uint32_t EGLBackend::getOrCreateFB(struct gbm_bo* bo) {
    if (!bo) return 0;
    uint32_t fbId = reinterpret_cast<uintptr_t>(gbm_bo_get_user_data(bo));
    if (fbId != 0) return fbId;

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;

    int ret = drmModeAddFB(m_drmFd, width, height, 24, 32, stride, handle, &fbId);
    if (ret != 0) {
        std::cerr << "[LCL EGL] drmModeAddFB failed for GBM buffer: " << std::strerror(errno) << "\n";
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

bool EGLBackend::swapBuffers() {
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
        drmModeSetCrtc(m_drmFd, m_crtcId, fbId, 0, 0, &m_connectorId, 1, NULL);
    }

    // Release previously rendered buffer back to GBM surface pool
    if (m_previousBO) {
        gbm_surface_release_buffer(m_gbmSurface, m_previousBO);
    }
    m_previousBO = bo;

    return true;
}

void EGLBackend::shutdown() {
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

    m_initialized = false;
}

} // namespace lcl::core
