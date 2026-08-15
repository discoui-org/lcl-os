#include "platform/android/android_input_backend.hpp"

namespace lcl::platform::android {

AndroidInputBackend::AndroidInputBackend() = default;

AndroidInputBackend::~AndroidInputBackend() {
    shutdown();
}

bool AndroidInputBackend::initialize(lcl::platform::InputEventCallback callback) {
    m_callback = std::move(callback);
    m_initialized = true;
    return true;
}

void AndroidInputBackend::shutdown() {
    m_callback = nullptr;
    m_initialized = false;
}

size_t AndroidInputBackend::pollEvents(int /*screenWidth*/, int /*screenHeight*/) {
    return 0;
}

} // namespace lcl::platform::android
