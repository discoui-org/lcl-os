#include "system/input/input_manager.hpp"

namespace lcl::core {

InputManager::InputManager() = default;

InputManager::InputManager(std::unique_ptr<lcl::platform::IInputBackend> backend)
    : m_backend(std::move(backend)) {}

InputManager::~InputManager() {
    shutdown();
}

InputManager::InputManager(InputManager&& other) noexcept
    : m_backend(std::move(other.m_backend)),
      m_eventCallback(std::move(other.m_eventCallback)),
      m_initialized(other.m_initialized) {
    other.m_initialized = false;
}

InputManager& InputManager::operator=(InputManager&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_backend = std::move(other.m_backend);
        m_eventCallback = std::move(other.m_eventCallback);
        m_initialized = other.m_initialized;
        other.m_initialized = false;
    }
    return *this;
}

bool InputManager::initialize(std::unique_ptr<lcl::platform::IInputBackend> backend) {
    if (m_initialized && m_backend) return true;

    if (backend) {
        m_backend = std::move(backend);
    }

    if (!m_backend) {
        return false;
    }

    m_initialized = m_backend->initialize([this](const lcl::platform::RawInputEvent& event) {
        if (m_eventCallback) {
            m_eventCallback(event);
        }
    });

    return m_initialized;
}

void InputManager::setEventCallback(EventCallback cb) {
    m_eventCallback = std::move(cb);
}

size_t InputManager::dispatchEvents(int screenWidth, int screenHeight) {
    if (!m_initialized || !m_backend) return 0;
    return m_backend->pollEvents(screenWidth, screenHeight);
}

void InputManager::shutdown() {
    if (m_backend && m_initialized) {
        m_backend->shutdown();
    }
    m_initialized = false;
}

} // namespace lcl::core
