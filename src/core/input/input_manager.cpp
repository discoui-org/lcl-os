#include "core/input/input_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace {

int open_restricted(const char* path, int flags, void* user_data) {
    (void)user_data;
    int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void close_restricted(int fd, void* user_data) {
    (void)user_data;
    close(fd);
}

const struct libinput_interface g_libinput_interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

} // namespace

namespace lcl::core {

InputManager::InputManager() = default;

InputManager::~InputManager() {
    shutdown();
}

InputManager::InputManager(InputManager&& other) noexcept
    : m_udev(other.m_udev),
      m_libinput(other.m_libinput),
      m_seatName(std::move(other.m_seatName)),
      m_initialized(other.m_initialized) {
    other.m_udev = nullptr;
    other.m_libinput = nullptr;
    other.m_initialized = false;
}

InputManager& InputManager::operator=(InputManager&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_udev = other.m_udev;
        m_libinput = other.m_libinput;
        m_seatName = std::move(other.m_seatName);
        m_initialized = other.m_initialized;

        other.m_udev = nullptr;
        other.m_libinput = nullptr;
        other.m_initialized = false;
    }
    return *this;
}

bool InputManager::initialize(const std::string& seatName) {
    if (m_initialized) {
        std::cout << "[LCL Input] InputManager already initialized.\n";
        return true;
    }

    m_seatName = seatName;
    std::cout << "[LCL Input] Initializing libinput / evdev context on seat: " << m_seatName << "...\n";

    m_udev = udev_new();
    if (!m_udev) {
        std::cerr << "[LCL Input ERROR] Failed to initialize udev context.\n";
        return false;
    }

    m_libinput = libinput_udev_create_context(&g_libinput_interface, nullptr, m_udev);
    if (!m_libinput) {
        std::cerr << "[LCL Input ERROR] Failed to create libinput udev context.\n";
        cleanup();
        return false;
    }

    if (libinput_udev_assign_seat(m_libinput, m_seatName.c_str()) != 0) {
        std::cerr << "[LCL Input WARNING] Failed to assign seat '" << m_seatName << "' to libinput.\n";
        std::cerr << "[LCL Input HINT] Device node access may require root or 'input' group permissions.\n";
        cleanup();
        return false;
    }

    m_initialized = true;
    std::cout << "[LCL Input] Input Subsystem successfully initialized on " << m_seatName << "!\n";
    return true;
}

int InputManager::getFD() const {
    if (m_libinput) {
        return libinput_get_fd(m_libinput);
    }
    return -1;
}

size_t InputManager::dispatchEvents() {
    if (!m_initialized || !m_libinput) return 0;

    libinput_dispatch(m_libinput);
    struct libinput_event* event = nullptr;
    size_t eventCount = 0;

    while ((event = libinput_get_event(m_libinput)) != nullptr) {
        enum libinput_event_type type = libinput_event_get_type(event);
        struct libinput_device* dev = libinput_event_get_device(event);
        const char* devName = dev ? libinput_device_get_name(dev) : "Unknown Device";

        switch (type) {
            case LIBINPUT_EVENT_KEYBOARD_KEY:
                std::cout << "[LCL Input Event] Keyboard Key Event from " << devName << "\n";
                break;
            case LIBINPUT_EVENT_POINTER_MOTION:
                std::cout << "[LCL Input Event] Pointer Motion Event from " << devName << "\n";
                break;
            case LIBINPUT_EVENT_POINTER_BUTTON:
                std::cout << "[LCL Input Event] Pointer Button Event from " << devName << "\n";
                break;
            case LIBINPUT_EVENT_TOUCH_DOWN:
                std::cout << "[LCL Input Event] Touch Down Event from " << devName << "\n";
                break;
            default:
                break;
        }

        libinput_event_destroy(event);
        libinput_dispatch(m_libinput);
        eventCount++;
    }

    return eventCount;
}

void InputManager::shutdown() {
    if (!m_initialized && !m_udev && !m_libinput) return;

    std::cout << "[LCL Input] Shutting down Input Subsystem...\n";
    cleanup();
    m_initialized = false;
}

void InputManager::cleanup() {
    if (m_libinput) {
        libinput_unref(m_libinput);
        m_libinput = nullptr;
    }
    if (m_udev) {
        udev_unref(m_udev);
        m_udev = nullptr;
    }
}

} // namespace lcl::core
