#include "platform/android/android_platform_services.hpp"
#include "platform/android/ahardware_native_buffer.hpp"

#include <android/hardware_buffer.h>
#include <cerrno>
#include <iostream>
#include <utility>

namespace lcl::platform::android {

AndroidPlatformServices::AndroidPlatformServices() = default;

AndroidPlatformServices::~AndroidPlatformServices() {
    shutdown();
}

bool AndroidPlatformServices::initialize() {
    if (m_initialized) return true;

    const auto gestaltResult = lcl::platform::loadGestalt(m_paths.gestaltFilePath());
    if (!gestaltResult.ok()) {
        std::cerr << "[AndroidPlatformServices] " << gestaltResult.error << "\n";
        return false;
    }
    m_gestalt = gestaltResult.gestalt;
    m_displayBackend.setGestalt(m_gestalt);
    if (gestaltResult.loadedFromFile) {
        std::cout << "[LCL Gestalt] Loaded " << gestaltResult.path
                  << " (" << m_gestalt.name << ")\n";
    } else {
        std::cout << "[LCL Gestalt] Using built-in default; no file at "
                  << gestaltResult.path << "\n";
    }

    // 1. Initialize Android Display Backend (Composer3 AIDL, then Composer 2.4/2.2 HIDL)
    if (!m_displayBackend.initialize()) {
        std::cerr << "[AndroidPlatformServices] Display backend initialization failed or running in fallback.\n";
    }

    // 2. Initialize Android Graphics Context (EGL/GLES 3) with active display dimensions
    const auto& mode = m_displayBackend.activeMode();
    uint32_t width = mode.width > 0 ? mode.width : 320;
    uint32_t height = mode.height > 0 ? mode.height : 640;

    if (!m_graphicsContext.initialize(width, height, &m_displayBackend)) {
        std::cerr << "[AndroidPlatformServices] Graphics context initialization failed.\n";
        return false;
    }

    // 3. Initialize Input Backend
    m_inputBackend.initialize(nullptr);

    m_initialized = true;
    return true;
}

void AndroidPlatformServices::shutdown() {
    if (!m_initialized) return;

    // Composer owns pending release fences for the scanout AHBs. Drain and
    // destroy its layer while those buffers are still alive, then tear down
    // their EGL/AHB storage.
    m_inputBackend.shutdown();
    m_displayBackend.shutdown();
    m_graphicsContext.shutdown();

    m_initialized = false;
}

lcl::platform::NativeBufferReceiveResult
AndroidPlatformServices::receiveNativeBuffer(int socketFd) {
    if (socketFd < 0) {
        return {lcl::platform::NativeBufferReceiveStatus::Error, {}, {}};
    }
    AHardwareBuffer* handle = nullptr;
    errno = 0;
    const int result = AHardwareBuffer_recvHandleFromUnixSocket(
        socketFd, &handle);
    if (result != 0 || !handle) {
        if (handle) AHardwareBuffer_release(handle);
        const int error = result < 0 ? -result : result;
        if (error == EAGAIN || error == EWOULDBLOCK ||
            (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            return {lcl::platform::NativeBufferReceiveStatus::WouldBlock,
                    {}, {}};
        }
        std::cerr << "[AndroidPlatformServices] Native-buffer receive failed"
                  << " (result=" << result << ", errno=" << errno << ")\n";
        return {lcl::platform::NativeBufferReceiveStatus::Error, {}, {}};
    }

    AHardwareBuffer_Desc ahbDescription{};
    AHardwareBuffer_describe(handle, &ahbDescription);
    lcl::platform::NativeBufferDescription description{};
    description.width = ahbDescription.width;
    description.height = ahbDescription.height;
    description.layers = ahbDescription.layers;
    description.format = ahbDescription.format;
    description.stridePixels = ahbDescription.stride;
    description.usage = ahbDescription.usage;
    auto buffer = std::make_shared<AHardwareNativeBuffer>(
        handle, AHardwareNativeBuffer::ReferenceMode::Adopt);
    return {lcl::platform::NativeBufferReceiveStatus::Received,
            std::move(buffer), description};
}

} // namespace lcl::platform::android
