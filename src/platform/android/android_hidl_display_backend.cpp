#include "platform/android/android_hidl_display_backend.hpp"
#include "platform/android/android_hidl_bridge.h"

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

#include <iostream>
#include <string>

namespace lcl::platform::android {

struct AndroidHidlDisplayBackend::Impl {
    using Create = void* (*)();
    using Destroy = void (*)(void*);
    using Initialize = int (*)(void*, float, uint32_t, uint32_t, uint32_t,
                               LclAndroidHidlDisplayInfo*);
    using Shutdown = void (*)(void*);
    using PrepareBuffer = int (*)(void*, AHardwareBuffer*);
    using Present = int (*)(void*, AHardwareBuffer*, int);
    using WaitVsync = int (*)(void*, int64_t);

    void* library{nullptr};
    void* instance{nullptr};
    Create create{nullptr};
    Destroy destroy{nullptr};
    Initialize initialize{nullptr};
    Shutdown shutdown{nullptr};
    PrepareBuffer prepareBuffer{nullptr};
    Present present{nullptr};
    WaitVsync waitVsync{nullptr};
};

namespace {

std::string siblingBridgePath() {
    char executable[PATH_MAX]{};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) return "liblcl-android-hidl-bridge.so";
    executable[length] = '\0';
    std::string path(executable);
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "liblcl-android-hidl-bridge.so";
    return path.substr(0, slash + 1) + "liblcl-android-hidl-bridge.so";
}

template <typename T>
bool loadSymbol(void* library, const char* name, T* destination) {
    *destination = reinterpret_cast<T>(dlsym(library, name));
    if (*destination) return true;
    std::cerr << "[AndroidHidlDisplayBackend] bridge symbol missing: " << name << "\n";
    return false;
}

} // namespace

AndroidHidlDisplayBackend::AndroidHidlDisplayBackend()
    : m_impl(std::make_unique<Impl>()) {
    m_activeMode.width = 320;
    m_activeMode.height = 640;
    m_activeMode.refreshRate = 60;
    m_activeMode.refreshRateHz = 60;
    m_activeMode.scaleFactor = 1.0f;
    m_activeMode.name = "Android HIDL Primary Display";
}

AndroidHidlDisplayBackend::~AndroidHidlDisplayBackend() {
    shutdown();
    if (m_impl->instance && m_impl->destroy) m_impl->destroy(m_impl->instance);
    if (m_impl->library) dlclose(m_impl->library);
}

bool AndroidHidlDisplayBackend::initialize(float outputScale,
                                           uint32_t preferredWidth,
                                           uint32_t preferredHeight,
                                           uint32_t preferredRefreshHz) {
    if (m_initialized) return true;
    if (!m_impl->library) {
        const std::string path = siblingBridgePath();
        m_impl->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!m_impl->library) {
            m_impl->library = dlopen("liblcl-android-hidl-bridge.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!m_impl->library) {
            std::cerr << "[AndroidHidlDisplayBackend] failed to load HIDL bridge: "
                      << dlerror() << "\n";
            return false;
        }
        if (!loadSymbol(m_impl->library, "lcl_android_hidl_create", &m_impl->create) ||
            !loadSymbol(m_impl->library, "lcl_android_hidl_destroy", &m_impl->destroy) ||
            !loadSymbol(m_impl->library, "lcl_android_hidl_initialize", &m_impl->initialize) ||
            !loadSymbol(m_impl->library, "lcl_android_hidl_shutdown", &m_impl->shutdown) ||
            !loadSymbol(m_impl->library, "lcl_android_hidl_prepare_buffer",
                        &m_impl->prepareBuffer) ||
            !loadSymbol(m_impl->library, "lcl_android_hidl_present", &m_impl->present) ||
            !loadSymbol(m_impl->library, "lcl_android_hidl_wait_vsync",
                        &m_impl->waitVsync)) {
            return false;
        }
        m_impl->instance = m_impl->create();
    }
    if (!m_impl->instance) return false;

    LclAndroidHidlDisplayInfo info{};
    if (!m_impl->initialize(m_impl->instance, outputScale,
                            preferredWidth, preferredHeight, preferredRefreshHz,
                            &info)) return false;
    m_activeMode.width = info.width;
    m_activeMode.height = info.height;
    m_activeMode.refreshRate = info.refresh_rate_hz;
    m_activeMode.refreshRateHz = info.refresh_rate_hz;
    m_activeMode.scaleFactor = info.scale_factor;
    m_activeMode.name = info.name;
    m_displayId = static_cast<uint64_t>(info.display_id);
    m_layerId = static_cast<uint64_t>(info.layer_id);
    m_displayConnected = info.connected != 0;
    m_hasLayer = info.layer_id >= 0;
    m_initialized = true;
    return true;
}

void AndroidHidlDisplayBackend::shutdown() {
    if (m_initialized && m_impl->instance && m_impl->shutdown) {
        m_impl->shutdown(m_impl->instance);
    }
    m_initialized = false;
    m_displayConnected = false;
    m_hasLayer = false;
    m_layerId = 0;
}

bool AndroidHidlDisplayBackend::prepareBufferForRender(AHardwareBuffer* buffer) {
    return m_initialized && m_impl->instance && m_impl->prepareBuffer &&
           m_impl->prepareBuffer(m_impl->instance, buffer) != 0;
}

bool AndroidHidlDisplayBackend::presentBuffer(AHardwareBuffer* buffer, int acquireFenceFd) {
    return m_initialized && m_impl->instance && m_impl->present &&
           m_impl->present(m_impl->instance, buffer, acquireFenceFd) != 0;
}

bool AndroidHidlDisplayBackend::waitForVsync(std::chrono::nanoseconds timeout) {
    return m_initialized && m_impl->instance && m_impl->waitVsync &&
           timeout.count() > 0 &&
           m_impl->waitVsync(m_impl->instance, timeout.count()) != 0;
}

} // namespace lcl::platform::android
