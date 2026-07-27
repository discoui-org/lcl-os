#include "core/input/input_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

namespace {

int li_open_restricted(const char* path, int flags, void*) {
    int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void li_close_restricted(int fd, void*) { close(fd); }

const struct libinput_interface g_libinput_iface = {
    .open_restricted = li_open_restricted,
    .close_restricted = li_close_restricted,
};

} // namespace

namespace lcl::core {

InputManager::InputManager() = default;
InputManager::~InputManager() { shutdown(); }

InputManager::InputManager(InputManager&& other) noexcept
    : m_udev(other.m_udev), m_libinput(other.m_libinput),
      m_evdevDevices(std::move(other.m_evdevDevices)),
      m_netlinkFd(other.m_netlinkFd),
      m_dispatchCounter(other.m_dispatchCounter),
      m_seatName(std::move(other.m_seatName)),
      m_eventCallback(std::move(other.m_eventCallback)),
      m_initialized(other.m_initialized), m_usingEvdev(other.m_usingEvdev) {
    other.m_udev = nullptr; other.m_libinput = nullptr;
    other.m_netlinkFd = -1;
    other.m_initialized = false; other.m_usingEvdev = false;
}

InputManager& InputManager::operator=(InputManager&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_udev = other.m_udev; m_libinput = other.m_libinput;
        m_evdevDevices = std::move(other.m_evdevDevices);
        m_netlinkFd = other.m_netlinkFd;
        m_dispatchCounter = other.m_dispatchCounter;
        m_seatName = std::move(other.m_seatName);
        m_eventCallback = std::move(other.m_eventCallback);
        m_initialized = other.m_initialized; m_usingEvdev = other.m_usingEvdev;
        other.m_udev = nullptr; other.m_libinput = nullptr;
        other.m_netlinkFd = -1;
        other.m_initialized = false; other.m_usingEvdev = false;
    }
    return *this;
}

bool InputManager::initialize(const std::string& seatName) {
    if (m_initialized) return true;
    m_seatName = seatName;
    std::cout << "[LCL Input] Initializing input subsystem...\n";

    if (initWithLibinputUdev(seatName)) return true;

    std::cout << "[LCL Input] Falling back to direct evdev backend...\n";
    if (initWithEvdev()) return true;

    std::cerr << "[LCL Input ERROR] All input backends exhausted.\n";
    return false;
}

bool InputManager::initWithLibinputUdev(const std::string& seatName) {
    m_udev = udev_new();
    if (!m_udev) return false;

    struct libinput* li = libinput_udev_create_context(&g_libinput_iface, nullptr, m_udev);
    if (!li) { udev_unref(m_udev); m_udev = nullptr; return false; }

    if (libinput_udev_assign_seat(li, seatName.c_str()) != 0) {
        libinput_unref(li); udev_unref(m_udev); m_udev = nullptr;
        return false;
    }

    // Verify devices were actually enumerated
    libinput_dispatch(li);
    int devCount = 0;
    struct libinput_event* ev = nullptr;
    while ((ev = libinput_get_event(li)) != nullptr) {
        if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED) devCount++;
        libinput_event_destroy(ev);
    }

    if (devCount == 0) {
        std::cout << "[LCL Input] libinput/udev: no devices enumerated (udevd absent).\n";
        libinput_unref(li); udev_unref(m_udev); m_udev = nullptr;
        return false;
    }

    m_libinput = li;
    m_usingEvdev = false;
    m_initialized = true;
    std::cout << "[LCL Input] libinput/udev backend: " << devCount << " device(s) on " << seatName << ".\n";
    return true;
}

bool InputManager::initUeventSocket() {
    if (m_netlinkFd >= 0) return true;
    m_netlinkFd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (m_netlinkFd < 0) return false;

    struct sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1; // Broadcast group 1

    if (bind(m_netlinkFd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        close(m_netlinkFd);
        m_netlinkFd = -1;
        return false;
    }

    int flags = fcntl(m_netlinkFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(m_netlinkFd, F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

void InputManager::processUeventHotplug() {
    if (m_netlinkFd < 0) return;
    char buf[4096];
    while (true) {
        ssize_t len = recv(m_netlinkFd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (len <= 0) break;
        buf[len] = '\0';

        std::string ueventStr(buf, static_cast<size_t>(len));
        bool isAdd = (ueventStr.find("add@") != std::string::npos) ||
                     (ueventStr.find("ACTION=add") != std::string::npos);
        bool isInput = (ueventStr.find("SUBSYSTEM=input") != std::string::npos);

        if (isAdd && isInput) {
            rescanEvdevDevices();
        }
    }
}

void InputManager::performPeriodicRescan() {
    m_dispatchCounter++;
    if (m_evdevDevices.empty() || (m_dispatchCounter % 100 == 0)) {
        rescanEvdevDevices();
    }
}

size_t InputManager::rescanEvdevDevices() {
    const std::string inputDir = "/dev/input";
    DIR* dir = opendir(inputDir.c_str());
    if (!dir) return 0;

    size_t newCount = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.rfind("event", 0) != 0) continue;

        std::string path = inputDir + "/" + name;
        bool alreadyOpened = false;
        for (const auto& dev : m_evdevDevices) {
            if (dev.path == path && dev.fd >= 0) {
                alreadyOpened = true;
                break;
            }
        }
        if (alreadyOpened) continue;

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        EvdevDevice dev{};
        dev.fd = fd;
        dev.path = path;

        char devname[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(devname)), devname) >= 0) {
            dev.name = devname;
        }

        uint8_t relBits[KEY_MAX / 8 + 1] = {};
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits) >= 0) {
            dev.hasRelX = (relBits[REL_X / 8] >> (REL_X % 8)) & 1;
            dev.hasRelY = (relBits[REL_Y / 8] >> (REL_Y % 8)) & 1;
        }

        uint8_t absBits[KEY_MAX / 8 + 1] = {};
        bool hasMTAbsX = false;
        bool hasMTAbsY = false;
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0) {
            dev.hasAbsX = (absBits[ABS_X / 8] >> (ABS_X % 8)) & 1;
            dev.hasAbsY = (absBits[ABS_Y / 8] >> (ABS_Y % 8)) & 1;

            hasMTAbsX = (absBits[ABS_MT_POSITION_X / 8] >> (ABS_MT_POSITION_X % 8)) & 1;
            hasMTAbsY = (absBits[ABS_MT_POSITION_Y / 8] >> (ABS_MT_POSITION_Y % 8)) & 1;

            int axisX = dev.hasAbsX ? ABS_X : (hasMTAbsX ? ABS_MT_POSITION_X : -1);
            int axisY = dev.hasAbsY ? ABS_Y : (hasMTAbsY ? ABS_MT_POSITION_Y : -1);

            if (axisX >= 0) {
                struct input_absinfo absinfo{};
                if (ioctl(fd, EVIOCGABS(axisX), &absinfo) >= 0) {
                    dev.absXMin = absinfo.minimum;
                    dev.absXMax = std::max(absinfo.maximum, absinfo.minimum + 1);
                }
            }
            if (axisY >= 0) {
                struct input_absinfo absinfo{};
                if (ioctl(fd, EVIOCGABS(axisY), &absinfo) >= 0) {
                    dev.absYMin = absinfo.minimum;
                    dev.absYMax = std::max(absinfo.maximum, absinfo.minimum + 1);
                }
            }

            if (hasMTAbsX) dev.hasAbsX = true;
            if (hasMTAbsY) dev.hasAbsY = true;
        }

        uint8_t keyBits[KEY_MAX / 8 + 1] = {};
        bool hasTouch = false;
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0) {
            hasTouch = ((keyBits[BTN_TOUCH / 8] >> (BTN_TOUCH % 8)) & 1) ||
                       ((keyBits[BTN_TOOL_FINGER / 8] >> (BTN_TOOL_FINGER % 8)) & 1);
        }

        if (hasTouch || (dev.hasAbsX && dev.hasAbsY && !dev.hasRelX)) {
            dev.isTouchpad = true;
        }

        if (!dev.hasRelX && !dev.hasRelY && !dev.hasAbsX && !dev.hasAbsY) {
            uint8_t evBits[KEY_MAX / 8 + 1] = {};
            ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);
            bool hasKey = (evBits[EV_KEY / 8] >> (EV_KEY % 8)) & 1;
            if (!hasKey) { close(fd); continue; }
        }

        m_evdevDevices.push_back(dev);
        newCount++;
        std::cout << "[LCL Input] evdev: Opened " << path << " [" << dev.name << "]"
                  << " rel(" << dev.hasRelX << "," << dev.hasRelY << ")"
                  << " abs(" << dev.hasAbsX << "," << dev.hasAbsY << ")"
                  << " touchpad(" << dev.isTouchpad << ")\n";
    }
    closedir(dir);
    return newCount;
}

bool InputManager::initWithEvdev() {
    initUeventSocket();
    rescanEvdevDevices();

    if (m_evdevDevices.empty()) {
        std::cerr << "[LCL Input ERROR] No evdev devices accessible yet (will retry via uevent/hotplug).\n";
    }

    m_usingEvdev = true;
    m_initialized = true;
    std::cout << "[LCL Input] evdev backend: " << m_evdevDevices.size() << " device(s) active.\n";
    return true;
}

int InputManager::getFD() const {
    if (m_libinput) return libinput_get_fd(m_libinput);
    if (!m_evdevDevices.empty()) return m_evdevDevices[0].fd;
    return -1;
}

size_t InputManager::dispatchEvents(int screenWidth, int screenHeight) {
    if (!m_initialized) return 0;
    if (!m_usingEvdev) return dispatchLibinputEvents(screenWidth, screenHeight);
    return dispatchEvdevEvents(screenWidth, screenHeight);
}

size_t InputManager::dispatchLibinputEvents(int screenWidth, int screenHeight) {
    if (!m_libinput) return 0;
    libinput_dispatch(m_libinput);
    struct libinput_event* event = nullptr;
    size_t count = 0;

    while ((event = libinput_get_event(m_libinput)) != nullptr) {
        auto type = libinput_event_get_type(event);
        struct libinput_device* dev = libinput_event_get_device(event);
        InputEvent outEv{};
        outEv.deviceName = dev ? libinput_device_get_name(dev) : "";

        switch (type) {
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* p = libinput_event_get_pointer_event(event);
                outEv.type = InputEventType::PointerMotion;
                outEv.dx = libinput_event_pointer_get_dx(p);
                outEv.dy = libinput_event_pointer_get_dy(p);
                if (m_eventCallback) m_eventCallback(outEv);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
                auto* p = libinput_event_get_pointer_event(event);
                outEv.type = InputEventType::PointerMotion;
                outEv.absoluteX = libinput_event_pointer_get_absolute_x_transformed(p, screenWidth);
                outEv.absoluteY = libinput_event_pointer_get_absolute_y_transformed(p, screenHeight);
                if (m_eventCallback) m_eventCallback(outEv);
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* p = libinput_event_get_pointer_event(event);
                outEv.type = InputEventType::PointerButton;
                outEv.button = libinput_event_pointer_get_button(p);
                outEv.pressed = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;
                if (m_eventCallback) m_eventCallback(outEv);
                break;
            }
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                auto* k = libinput_event_get_keyboard_event(event);
                outEv.type = InputEventType::KeyboardKey;
                outEv.key = libinput_event_keyboard_get_key(k);
                outEv.pressed = libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
                if (m_eventCallback) m_eventCallback(outEv);
                break;
            }
            default: break;
        }
        libinput_event_destroy(event);
        libinput_dispatch(m_libinput);
        count++;
    }
    return count;
}

size_t InputManager::dispatchEvdevEvents(int screenWidth, int screenHeight) {
    processUeventHotplug();
    performPeriodicRescan();

    if (m_evdevDevices.empty()) return 0;

    std::vector<struct pollfd> fds;
    fds.reserve(m_evdevDevices.size() + (m_netlinkFd >= 0 ? 1 : 0));
    for (auto& d : m_evdevDevices) {
        fds.push_back({d.fd, POLLIN, 0});
    }

    size_t netlinkIdx = fds.size();
    if (m_netlinkFd >= 0) {
        fds.push_back({m_netlinkFd, POLLIN, 0});
    }

    int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), 0);
    if (ready <= 0) return 0;

    if (m_netlinkFd >= 0 && (fds[netlinkIdx].revents & POLLIN)) {
        processUeventHotplug();
    }

    size_t count = 0;
    for (size_t i = 0; i < m_evdevDevices.size(); ++i) {
        if (!(fds[i].revents & POLLIN)) continue;

        auto& dev = m_evdevDevices[i];
        struct input_event ev{};

        while (read(dev.fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
            if (ev.type == EV_REL) {
                InputEvent outEv{};
                outEv.type = InputEventType::PointerMotion;
                outEv.deviceName = dev.name;
                if (ev.code == REL_X) outEv.dx = ev.value;
                if (ev.code == REL_Y) outEv.dy = ev.value;
                if ((ev.code == REL_X || ev.code == REL_Y) && m_eventCallback) {
                    m_eventCallback(outEv);
                }
                count++;
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                    dev.currentAbsX = ev.value;
                    dev.absXUpdated = true;
                }
                if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                    dev.currentAbsY = ev.value;
                    dev.absYUpdated = true;
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                if ((dev.absXUpdated || dev.absYUpdated) && (dev.hasAbsX || dev.hasAbsY)) {
                    if (dev.isTouchpad) {
                        if (dev.isTouching) {
                            double rangeX = static_cast<double>(dev.absXMax - dev.absXMin);
                            double rangeY = static_cast<double>(dev.absYMax - dev.absYMin);
                            if (rangeX <= 0.0) rangeX = 1.0;
                            if (rangeY <= 0.0) rangeY = 1.0;

                            double dx = 0.0;
                            double dy = 0.0;

                            if (dev.lastTouchX >= 0 && dev.currentAbsX >= 0) {
                                dx = (static_cast<double>(dev.currentAbsX - dev.lastTouchX) / rangeX) * screenWidth * 1.5;
                            }
                            if (dev.lastTouchY >= 0 && dev.currentAbsY >= 0) {
                                dy = (static_cast<double>(dev.currentAbsY - dev.lastTouchY) / rangeY) * screenHeight * 1.5;
                            }

                            dev.lastTouchX = dev.currentAbsX;
                            dev.lastTouchY = dev.currentAbsY;

                            if ((dx != 0.0 || dy != 0.0) && m_eventCallback) {
                                InputEvent outEv{};
                                outEv.type = InputEventType::PointerMotion;
                                outEv.deviceName = dev.name;
                                outEv.dx = dx;
                                outEv.dy = dy;
                                m_eventCallback(outEv);
                            }
                        }
                    } else {
                        InputEvent outEv{};
                        outEv.type = InputEventType::PointerMotion;
                        outEv.deviceName = dev.name;

                        double rangeX = static_cast<double>(dev.absXMax - dev.absXMin);
                        double rangeY = static_cast<double>(dev.absYMax - dev.absYMin);
                        if (rangeX <= 0.0) rangeX = 1.0;
                        if (rangeY <= 0.0) rangeY = 1.0;

                        outEv.absoluteX = (dev.currentAbsX >= 0) ?
                            (static_cast<double>(dev.currentAbsX - dev.absXMin) / rangeX * screenWidth) : -1.0;
                        outEv.absoluteY = (dev.currentAbsY >= 0) ?
                            (static_cast<double>(dev.currentAbsY - dev.absYMin) / rangeY * screenHeight) : -1.0;

                        if (m_eventCallback) m_eventCallback(outEv);
                    }
                    dev.absXUpdated = false;
                    dev.absYUpdated = false;
                    count++;
                }
            } else if (ev.type == EV_KEY) {
                if (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA) {
                    m_superPressed = (ev.value != 0);
                }

                InputEvent outEv{};
                outEv.deviceName = dev.name;
                outEv.pressed = (ev.value != 0);
                outEv.isRepeat = (ev.value == 2);
                outEv.superPressed = m_superPressed;

                if (ev.code == BTN_TOUCH || ev.code == BTN_TOOL_FINGER) {
                    outEv.type = InputEventType::PointerButton;
                    outEv.button = BTN_LEFT;
                    if (ev.value != 0) {
                        dev.isTouching = true;
                        dev.lastTouchX = dev.currentAbsX;
                        dev.lastTouchY = dev.currentAbsY;
                    } else {
                        dev.isTouching = false;
                        dev.lastTouchX = -1;
                        dev.lastTouchY = -1;
                    }
                    if (m_eventCallback) m_eventCallback(outEv);
                    count++;
                } else if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                    outEv.type = InputEventType::PointerButton;
                    outEv.button = ev.code;
                    if (m_eventCallback) m_eventCallback(outEv);
                    count++;
                } else {
                    outEv.type = InputEventType::KeyboardKey;
                    outEv.key = ev.code;
                    if (m_eventCallback) m_eventCallback(outEv);
                    count++;
                }
            }
        }
    }
    return count;
}

void InputManager::shutdown() {
    if (!m_initialized && !m_udev && !m_libinput && m_evdevDevices.empty()) return;
    std::cout << "[LCL Input] Shutting down Input Subsystem...\n";
    cleanup();
    m_initialized = false;
}

void InputManager::cleanup() {
    for (auto& d : m_evdevDevices) {
        if (d.fd >= 0) { close(d.fd); d.fd = -1; }
    }
    m_evdevDevices.clear();

    if (m_netlinkFd >= 0) {
        close(m_netlinkFd);
        m_netlinkFd = -1;
    }

    if (m_libinput) { libinput_unref(m_libinput); m_libinput = nullptr; }
    if (m_udev) { udev_unref(m_udev); m_udev = nullptr; }
}

} // namespace lcl::core
