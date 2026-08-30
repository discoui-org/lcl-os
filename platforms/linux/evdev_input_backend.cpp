#include "platforms/linux/evdev_input_backend.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int li_open_restricted(const char* path, int flags, void*) {
    int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void li_close_restricted(int fd, void*) {
    close(fd);
}

const struct libinput_interface g_libinput_iface = {
    .open_restricted = li_open_restricted,
    .close_restricted = li_close_restricted,
};

} // namespace

namespace lcl::platform::desktop {

EvdevInputBackend::EvdevInputBackend() = default;

EvdevInputBackend::~EvdevInputBackend() {
    shutdown();
}

EvdevInputBackend::EvdevInputBackend(EvdevInputBackend&& other) noexcept
    : m_udev(other.m_udev),
      m_libinput(other.m_libinput),
      m_evdevDevices(std::move(other.m_evdevDevices)),
      m_netlinkFd(other.m_netlinkFd),
      m_dispatchCounter(other.m_dispatchCounter),
      m_seatName(std::move(other.m_seatName)),
      m_callback(std::move(other.m_callback)),
      m_initialized(other.m_initialized),
      m_usingEvdev(other.m_usingEvdev),
      m_superPressed(other.m_superPressed),
      m_shiftPressed(other.m_shiftPressed),
      m_ctrlPressed(other.m_ctrlPressed),
      m_altPressed(other.m_altPressed),
      m_capsLockActive(other.m_capsLockActive) {
    other.m_udev = nullptr;
    other.m_libinput = nullptr;
    other.m_netlinkFd = -1;
    other.m_initialized = false;
    other.m_usingEvdev = false;
}

EvdevInputBackend& EvdevInputBackend::operator=(EvdevInputBackend&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_udev = other.m_udev;
        m_libinput = other.m_libinput;
        m_evdevDevices = std::move(other.m_evdevDevices);
        m_netlinkFd = other.m_netlinkFd;
        m_dispatchCounter = other.m_dispatchCounter;
        m_seatName = std::move(other.m_seatName);
        m_callback = std::move(other.m_callback);
        m_initialized = other.m_initialized;
        m_usingEvdev = other.m_usingEvdev;
        m_superPressed = other.m_superPressed;
        m_shiftPressed = other.m_shiftPressed;
        m_ctrlPressed = other.m_ctrlPressed;
        m_altPressed = other.m_altPressed;
        m_capsLockActive = other.m_capsLockActive;

        other.m_udev = nullptr;
        other.m_libinput = nullptr;
        other.m_netlinkFd = -1;
        other.m_initialized = false;
        other.m_usingEvdev = false;
    }
    return *this;
}

bool EvdevInputBackend::initialize(lcl::platform::InputEventCallback callback) {
    return initialize("seat0", std::move(callback));
}

bool EvdevInputBackend::initialize(const std::string& seatName, lcl::platform::InputEventCallback callback) {
    if (m_initialized) return true;
    m_seatName = seatName;
    m_callback = std::move(callback);
    std::cout << "[LCL Input] Initializing input subsystem...\n";

    if (initWithLibinputUdev(seatName)) {
        return true;
    }

    std::cout << "[LCL Input] Falling back to direct evdev backend...\n";
    if (initWithEvdev()) {
        return true;
    }

    std::cerr << "[LCL Input ERROR] All input backends exhausted.\n";
    return false;
}

bool EvdevInputBackend::initWithLibinputUdev(const std::string& seatName) {
    m_udev = udev_new();
    if (!m_udev) return false;

    struct libinput* li = libinput_udev_create_context(&g_libinput_iface, nullptr, m_udev);
    if (!li) {
        udev_unref(m_udev);
        m_udev = nullptr;
        return false;
    }

    if (libinput_udev_assign_seat(li, seatName.c_str()) != 0) {
        libinput_unref(li);
        udev_unref(m_udev);
        m_udev = nullptr;
        return false;
    }

    // Verify devices were actually enumerated
    libinput_dispatch(li);
    int devCount = 0;
    struct libinput_event* ev = nullptr;
    while ((ev = libinput_get_event(li)) != nullptr) {
        if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED) {
            devCount++;
        }
        libinput_event_destroy(ev);
    }

    if (devCount == 0) {
        std::cout << "[LCL Input] libinput/udev: no devices enumerated (udevd absent).\n";
        libinput_unref(li);
        udev_unref(m_udev);
        m_udev = nullptr;
        return false;
    }

    m_libinput = li;
    m_usingEvdev = false;
    m_initialized = true;
    std::cout << "[LCL Input] libinput/udev backend: " << devCount << " device(s) on " << seatName << ".\n";
    return true;
}

bool EvdevInputBackend::initUeventSocket() {
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

void EvdevInputBackend::processUeventHotplug() {
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

void EvdevInputBackend::performPeriodicRescan() {
    m_dispatchCounter++;
    if (m_evdevDevices.empty() || (m_dispatchCounter % 100 == 0)) {
        rescanEvdevDevices();
    }
}

size_t EvdevInputBackend::rescanEvdevDevices() {
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

        // Property bits
        uint8_t propBits[INPUT_PROP_MAX / 8 + 1] = {};
        bool hasPropDirect = false;
        bool hasPropPointer = false;
        if (ioctl(fd, EVIOCGPROP(sizeof(propBits)), propBits) >= 0) {
            hasPropDirect = (propBits[INPUT_PROP_DIRECT / 8] >> (INPUT_PROP_DIRECT % 8)) & 1;
            hasPropPointer = (propBits[INPUT_PROP_POINTER / 8] >> (INPUT_PROP_POINTER % 8)) & 1;
        }

        uint8_t relBits[KEY_MAX / 8 + 1] = {};
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits) >= 0) {
            dev.hasRelX = (relBits[REL_X / 8] >> (REL_X % 8)) & 1;
            dev.hasRelY = (relBits[REL_Y / 8] >> (REL_Y % 8)) & 1;
        }

        uint8_t absBits[KEY_MAX / 8 + 1] = {};
        bool hasMTAbsX = false;
        bool hasMTAbsY = false;
        bool hasMTTrackingId = false;
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0) {
            dev.hasAbsX = (absBits[ABS_X / 8] >> (ABS_X % 8)) & 1;
            dev.hasAbsY = (absBits[ABS_Y / 8] >> (ABS_Y % 8)) & 1;

            hasMTAbsX = (absBits[ABS_MT_POSITION_X / 8] >> (ABS_MT_POSITION_X % 8)) & 1;
            hasMTAbsY = (absBits[ABS_MT_POSITION_Y / 8] >> (ABS_MT_POSITION_Y % 8)) & 1;
            hasMTTrackingId = (absBits[ABS_MT_TRACKING_ID / 8] >> (ABS_MT_TRACKING_ID % 8)) & 1;

            int axisX = hasMTAbsX ? ABS_MT_POSITION_X : (dev.hasAbsX ? ABS_X : -1);
            int axisY = hasMTAbsY ? ABS_MT_POSITION_Y : (dev.hasAbsY ? ABS_Y : -1);

            if (axisX >= 0) {
                struct input_absinfo absinfo{};
                if (ioctl(fd, EVIOCGABS(axisX), &absinfo) >= 0) {
                    dev.absXMin = absinfo.minimum;
                    dev.absXMax = std::max(absinfo.maximum, absinfo.minimum + 1);
                    dev.currentAbsX = absinfo.value;
                }
                if (dev.absXMax <= dev.absXMin + 1 && dev.hasAbsX && axisX != ABS_X) {
                    if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) >= 0 && absinfo.maximum > absinfo.minimum) {
                        dev.absXMin = absinfo.minimum;
                        dev.absXMax = absinfo.maximum;
                    }
                }
            }
            if (axisY >= 0) {
                struct input_absinfo absinfo{};
                if (ioctl(fd, EVIOCGABS(axisY), &absinfo) >= 0) {
                    dev.absYMin = absinfo.minimum;
                    dev.absYMax = std::max(absinfo.maximum, absinfo.minimum + 1);
                    dev.currentAbsY = absinfo.value;
                }
                if (dev.absYMax <= dev.absYMin + 1 && dev.hasAbsY && axisY != ABS_Y) {
                    if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) >= 0 && absinfo.maximum > absinfo.minimum) {
                        dev.absYMin = absinfo.minimum;
                        dev.absYMax = absinfo.maximum;
                    }
                }
            }

            if (hasMTAbsX) dev.hasAbsX = true;
            if (hasMTAbsY) dev.hasAbsY = true;
        }

        const bool isMTTouchscreen = (hasMTAbsX && hasMTAbsY && hasMTTrackingId && !hasPropPointer);
        const bool isExplicitDirect = (hasPropDirect && (hasMTAbsX || hasMTAbsY || (dev.hasAbsX && dev.hasAbsY)));
        const bool isSingleTouchscreen = (dev.hasAbsX && dev.hasAbsY && !hasPropPointer && !hasMTAbsX && hasPropDirect);

        if (isMTTouchscreen || isExplicitDirect || isSingleTouchscreen) {
            dev.isDirectTouchscreen = true;
        } else if (hasMTAbsX || hasMTAbsY || hasPropPointer) {
            dev.isTouchpad = true;
        }

        if (!dev.hasRelX && !dev.hasRelY && !dev.hasAbsX && !dev.hasAbsY) {
            uint8_t evBits[KEY_MAX / 8 + 1] = {};
            ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);
            bool hasKey = (evBits[EV_KEY / 8] >> (EV_KEY % 8)) & 1;
            if (!hasKey) {
                close(fd);
                continue;
            }
        }

        m_evdevDevices.push_back(dev);
        newCount++;
    }
    closedir(dir);
    return newCount;
}

bool EvdevInputBackend::initWithEvdev() {
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

int EvdevInputBackend::getFd() const {
    if (m_libinput) return libinput_get_fd(m_libinput);
    if (!m_evdevDevices.empty()) return m_evdevDevices[0].fd;
    return -1;
}

size_t EvdevInputBackend::pollEvents(int screenWidth, int screenHeight) {
    if (!m_initialized) return 0;
    if (!m_usingEvdev) {
        return dispatchLibinputEvents(screenWidth, screenHeight);
    }
    return dispatchEvdevEvents(screenWidth, screenHeight);
}

size_t EvdevInputBackend::dispatchLibinputEvents(int screenWidth, int screenHeight) {
    if (!m_libinput) return 0;
    libinput_dispatch(m_libinput);
    struct libinput_event* event = nullptr;
    size_t count = 0;

    while ((event = libinput_get_event(m_libinput)) != nullptr) {
        auto type = libinput_event_get_type(event);
        struct libinput_device* dev = libinput_event_get_device(event);
        RawInputEvent outEv{};
        outEv.deviceName = dev ? libinput_device_get_name(dev) : "";

        switch (type) {
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* p = libinput_event_get_pointer_event(event);
                outEv.type = RawInputEventType::PointerMotion;
                outEv.source = PointerSource::Mouse;
                outEv.dx = libinput_event_pointer_get_dx(p);
                outEv.dy = libinput_event_pointer_get_dy(p);
                outEv.superPressed = m_superPressed;
                outEv.modifiers = getActiveModifiers();
                if (m_callback) m_callback(outEv);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
                auto* p = libinput_event_get_pointer_event(event);
                outEv.type = RawInputEventType::PointerMotion;
                outEv.source = PointerSource::Mouse;
                outEv.absoluteX = libinput_event_pointer_get_absolute_x_transformed(p, screenWidth);
                outEv.absoluteY = libinput_event_pointer_get_absolute_y_transformed(p, screenHeight);
                outEv.superPressed = m_superPressed;
                outEv.modifiers = getActiveModifiers();
                if (m_callback) m_callback(outEv);
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* p = libinput_event_get_pointer_event(event);
                outEv.type = RawInputEventType::PointerButton;
                outEv.source = PointerSource::Mouse;
                outEv.button = EvdevKeyMapper::toPointerButton(libinput_event_pointer_get_button(p));
                outEv.pressed = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;
                outEv.superPressed = m_superPressed;
                outEv.modifiers = getActiveModifiers();
                if (m_callback) m_callback(outEv);
                break;
            }
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                auto* k = libinput_event_get_keyboard_event(event);
                uint32_t linuxKey = libinput_event_keyboard_get_key(k);
                PhysicalKey physKey = EvdevKeyMapper::toPhysicalKey(linuxKey);
                bool pressed = libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;

                if (physKey == PhysicalKey::LeftShift || physKey == PhysicalKey::RightShift) {
                    m_shiftPressed = pressed;
                } else if (physKey == PhysicalKey::LeftCtrl || physKey == PhysicalKey::RightCtrl) {
                    m_ctrlPressed = pressed;
                } else if (physKey == PhysicalKey::LeftAlt || physKey == PhysicalKey::RightAlt) {
                    m_altPressed = pressed;
                } else if (physKey == PhysicalKey::LeftMeta || physKey == PhysicalKey::RightMeta) {
                    m_superPressed = pressed;
                } else if (physKey == PhysicalKey::CapsLock && pressed) {
                    m_capsLockActive = !m_capsLockActive;
                }

                outEv.type = RawInputEventType::KeyboardKey;
                outEv.key = physKey;
                outEv.pressed = pressed;
                outEv.superPressed = m_superPressed;
                outEv.modifiers = getActiveModifiers();
                outEv.codepoint = KeyboardMapper::toCodepoint(physKey, outEv.modifiers);
                if (m_callback) m_callback(outEv);
                break;
            }
            case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            case LIBINPUT_EVENT_POINTER_AXIS: {
                auto* p = libinput_event_get_pointer_event(event);
                outEv.type = RawInputEventType::PointerScroll;
                outEv.source = PointerSource::Mouse;
                if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
                    outEv.dy = libinput_event_pointer_get_axis_value(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                }
                if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
                    outEv.dx = libinput_event_pointer_get_axis_value(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
                }
                outEv.superPressed = m_superPressed;
                outEv.modifiers = getActiveModifiers();
                if (m_callback) m_callback(outEv);
                break;
            }
            default:
                break;
        }
        libinput_event_destroy(event);
        libinput_dispatch(m_libinput);
        count++;
    }
    return count;
}

size_t EvdevInputBackend::dispatchEvdevEvents(int screenWidth, int screenHeight) {
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
                if (ev.code == REL_X) {
                    dev.currentRelX += ev.value;
                    dev.relXUpdated = true;
                }
                if (ev.code == REL_Y) {
                    dev.currentRelY += ev.value;
                    dev.relYUpdated = true;
                }
                if (ev.code == REL_WHEEL) {
                    // In evdev, positive value is wheel up, negative is wheel down.
                    // For UI scrolling, deltaY > 0 scrolls content down, deltaY < 0 scrolls up.
                    dev.currentWheelY += -static_cast<double>(ev.value);
                    dev.wheelUpdated = true;
                }
#ifdef REL_WHEEL_HI_RES
                if (ev.code == REL_WHEEL_HI_RES) {
                    dev.currentWheelY += -static_cast<double>(ev.value) / 120.0;
                    dev.wheelUpdated = true;
                }
#endif
                if (ev.code == REL_HWHEEL) {
                    dev.currentWheelX += static_cast<double>(ev.value);
                    dev.wheelUpdated = true;
                }
#ifdef REL_HWHEEL_HI_RES
                if (ev.code == REL_HWHEEL_HI_RES) {
                    dev.currentWheelX += static_cast<double>(ev.value) / 120.0;
                    dev.wheelUpdated = true;
                }
#endif
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                    dev.currentAbsX = ev.value;
                    dev.absXUpdated = true;
                }
                if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                    dev.currentAbsY = ev.value;
                    dev.absYUpdated = true;
                }
                if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0) {
                        dev.isTouching = true;
                        if (dev.isDirectTouchscreen) {
                            dev.touchPressed = true;
                        }
                    } else {
                        dev.isTouching = false;
                        if (dev.isDirectTouchscreen) {
                            dev.touchReleased = true;
                        }
                        dev.lastTouchX = -1;
                        dev.lastTouchY = -1;
                    }
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                const uint64_t timestampNs =
                    static_cast<uint64_t>(ev.time.tv_sec) * 1000000000ull +
                    static_cast<uint64_t>(ev.time.tv_usec) * 1000ull;
                if (dev.isDirectTouchscreen) {
                    const double rangeX = static_cast<double>(dev.absXMax - dev.absXMin);
                    const double rangeY = static_cast<double>(dev.absYMax - dev.absYMin);

                    double normX = -1.0;
                    double normY = -1.0;
                    if (rangeX > 0.0 && dev.currentAbsX >= 0) {
                        normX = (static_cast<double>(dev.currentAbsX - dev.absXMin) / rangeX) * screenWidth;
                        normX = std::clamp(normX, 0.0, static_cast<double>(screenWidth - 1));
                    } else {
                        normX = dev.lastNormTouchX;
                    }

                    if (rangeY > 0.0 && dev.currentAbsY >= 0) {
                        normY = (static_cast<double>(dev.currentAbsY - dev.absYMin) / rangeY) * screenHeight;
                        normY = std::clamp(normY, 0.0, static_cast<double>(screenHeight - 1));
                    } else {
                        normY = dev.lastNormTouchY;
                    }

                    if (dev.touchPressed) {
                        if (normX >= 0.0 && normY >= 0.0) {
                            RawInputEvent moveEv{};
                            moveEv.type = RawInputEventType::PointerMotion;
                            moveEv.source = PointerSource::Touch;
                            moveEv.absoluteX = normX;
                            moveEv.absoluteY = normY;
                            moveEv.timestampNs = timestampNs;
                            moveEv.deviceName = dev.name;
                            moveEv.superPressed = m_superPressed;
                            moveEv.modifiers = getActiveModifiers();
                            if (m_callback) m_callback(moveEv);
                        }

                        RawInputEvent downEv{};
                        downEv.type = RawInputEventType::PointerButton;
                        downEv.source = PointerSource::Touch;
                        downEv.button = PointerButton::Left;
                        downEv.pressed = true;
                        downEv.absoluteX = normX;
                        downEv.absoluteY = normY;
                        downEv.timestampNs = timestampNs;
                        downEv.deviceName = dev.name;
                        downEv.superPressed = m_superPressed;
                        downEv.modifiers = getActiveModifiers();
                        if (m_callback) m_callback(downEv);

                        dev.lastNormTouchX = normX;
                        dev.lastNormTouchY = normY;
                        dev.touchPressed = false;
                        count++;
                    } else if (dev.touchReleased) {
                        RawInputEvent upEv{};
                        upEv.type = RawInputEventType::PointerButton;
                        upEv.source = PointerSource::Touch;
                        upEv.button = PointerButton::Left;
                        upEv.pressed = false;
                        upEv.absoluteX = normX >= 0.0 ? normX : dev.lastNormTouchX;
                        upEv.absoluteY = normY >= 0.0 ? normY : dev.lastNormTouchY;
                        upEv.timestampNs = timestampNs;
                        upEv.deviceName = dev.name;
                        upEv.superPressed = m_superPressed;
                        upEv.modifiers = getActiveModifiers();
                        if (m_callback) m_callback(upEv);

                        dev.lastNormTouchX = -1.0;
                        dev.lastNormTouchY = -1.0;
                        dev.touchReleased = false;
                        count++;
                    } else if (dev.isTouching && (dev.absXUpdated || dev.absYUpdated)) {
                        if (normX >= 0.0 && normY >= 0.0) {
                            RawInputEvent moveEv{};
                            moveEv.type = RawInputEventType::PointerMotion;
                            moveEv.source = PointerSource::Touch;
                            moveEv.absoluteX = normX;
                            moveEv.absoluteY = normY;
                            moveEv.timestampNs = timestampNs;
                            moveEv.deviceName = dev.name;
                            moveEv.superPressed = m_superPressed;
                            moveEv.modifiers = getActiveModifiers();
                            if (m_callback) m_callback(moveEv);

                            dev.lastNormTouchX = normX;
                            dev.lastNormTouchY = normY;
                            count++;
                        }
                    }

                    dev.absXUpdated = false;
                    dev.absYUpdated = false;
                } else if (dev.relXUpdated || dev.relYUpdated) {
                    RawInputEvent outEv{};
                    outEv.type = RawInputEventType::PointerMotion;
                    outEv.source = PointerSource::Mouse;
                    outEv.deviceName = dev.name;
                    outEv.dx = dev.currentRelX;
                    outEv.dy = dev.currentRelY;
                    outEv.superPressed = m_superPressed;
                    outEv.modifiers = getActiveModifiers();
                    if (m_callback) m_callback(outEv);

                    dev.currentRelX = 0.0;
                    dev.currentRelY = 0.0;
                    dev.relXUpdated = false;
                    dev.relYUpdated = false;
                    count++;
                }

                if (dev.wheelUpdated) {
                    RawInputEvent outEv{};
                    outEv.type = RawInputEventType::PointerScroll;
                    outEv.source = PointerSource::Mouse;
                    outEv.deviceName = dev.name;
                    outEv.dx = dev.currentWheelX;
                    outEv.dy = dev.currentWheelY;
                    outEv.superPressed = m_superPressed;
                    outEv.modifiers = getActiveModifiers();
                    if (m_callback) m_callback(outEv);

                    dev.currentWheelX = 0.0;
                    dev.currentWheelY = 0.0;
                    dev.wheelUpdated = false;
                    count++;
                }

                if ((dev.absXUpdated || dev.absYUpdated) &&
                    (dev.hasAbsX || dev.hasAbsY)) {
                    if (dev.isTouchpad) {
                        if (!dev.isTouching && dev.currentAbsX >= 0 && dev.currentAbsY >= 0) {
                            dev.isTouching = true;
                        }

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

                            if ((dx != 0.0 || dy != 0.0) && m_callback) {
                                RawInputEvent outEv{};
                                outEv.type = RawInputEventType::PointerMotion;
                                outEv.source = PointerSource::Mouse;
                                outEv.deviceName = dev.name;
                                outEv.dx = dx;
                                outEv.dy = dy;
                                outEv.superPressed = m_superPressed;
                                outEv.modifiers = getActiveModifiers();
                                m_callback(outEv);
                            }
                        }
                    } else {
                        RawInputEvent outEv{};
                        outEv.type = RawInputEventType::PointerMotion;
                        outEv.source = PointerSource::Mouse;
                        outEv.deviceName = dev.name;

                        double rangeX = static_cast<double>(dev.absXMax - dev.absXMin);
                        double rangeY = static_cast<double>(dev.absYMax - dev.absYMin);
                        if (rangeX <= 0.0) rangeX = 1.0;
                        if (rangeY <= 0.0) rangeY = 1.0;

                        outEv.absoluteX = (dev.currentAbsX >= 0)
                            ? (static_cast<double>(dev.currentAbsX - dev.absXMin) / rangeX * screenWidth)
                            : -1.0;
                        outEv.absoluteY = (dev.currentAbsY >= 0)
                            ? (static_cast<double>(dev.currentAbsY - dev.absYMin) / rangeY * screenHeight)
                            : -1.0;
                        outEv.superPressed = m_superPressed;
                        outEv.modifiers = getActiveModifiers();

                        if (m_callback) m_callback(outEv);
                    }
                    dev.absXUpdated = false;
                    dev.absYUpdated = false;
                    count++;
                }
            } else if (ev.type == EV_KEY) {
                if (dev.isDirectTouchscreen && ev.code == BTN_TOUCH) {
                    if (ev.value != 0) {
                        dev.isTouching = true;
                        dev.touchPressed = true;
                    } else {
                        dev.isTouching = false;
                        dev.touchReleased = true;
                    }
                } else if (dev.isTouchpad && (ev.code == BTN_TOUCH || ev.code == BTN_TOOL_FINGER)) {
                    RawInputEvent outEv{};
                    outEv.deviceName = dev.name;
                    outEv.type = RawInputEventType::PointerButton;
                    outEv.source = PointerSource::Mouse;
                    outEv.button = PointerButton::Left;
                    outEv.pressed = (ev.value != 0);
                    outEv.superPressed = m_superPressed;
                    outEv.modifiers = getActiveModifiers();
                    if (ev.value != 0) {
                        dev.isTouching = true;
                        dev.lastTouchX = dev.currentAbsX;
                        dev.lastTouchY = dev.currentAbsY;
                    } else {
                        dev.isTouching = false;
                        dev.lastTouchX = -1;
                        dev.lastTouchY = -1;
                    }
                    if (m_callback) m_callback(outEv);
                    count++;
                } else if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                    RawInputEvent outEv{};
                    outEv.deviceName = dev.name;
                    outEv.type = RawInputEventType::PointerButton;
                    outEv.source = PointerSource::Mouse;
                    outEv.button = EvdevKeyMapper::toPointerButton(ev.code);
                    outEv.pressed = (ev.value != 0);
                    outEv.superPressed = m_superPressed;
                    outEv.modifiers = getActiveModifiers();
                    if (m_callback) m_callback(outEv);
                    count++;
                } else if (ev.code == BTN_TOUCH || ev.code == BTN_TOOL_FINGER) {
                    // Ignore non-touchpad touch bits
                } else {
                    PhysicalKey physKey = EvdevKeyMapper::toPhysicalKey(ev.code);
                    if (physKey == PhysicalKey::LeftShift || physKey == PhysicalKey::RightShift) {
                        m_shiftPressed = (ev.value != 0);
                    } else if (physKey == PhysicalKey::LeftCtrl || physKey == PhysicalKey::RightCtrl) {
                        m_ctrlPressed = (ev.value != 0);
                    } else if (physKey == PhysicalKey::LeftAlt || physKey == PhysicalKey::RightAlt) {
                        m_altPressed = (ev.value != 0);
                    } else if (physKey == PhysicalKey::LeftMeta || physKey == PhysicalKey::RightMeta) {
                        m_superPressed = (ev.value != 0);
                    } else if (physKey == PhysicalKey::CapsLock && ev.value == 1) {
                        m_capsLockActive = !m_capsLockActive;
                    }

                    RawInputEvent outEv{};
                    outEv.deviceName = dev.name;
                    outEv.type = RawInputEventType::KeyboardKey;
                    outEv.key = physKey;
                    outEv.pressed = (ev.value != 0);
                    outEv.isRepeat = (ev.value == 2);
                    outEv.superPressed = m_superPressed;
                    outEv.modifiers = getActiveModifiers();
                    outEv.codepoint = KeyboardMapper::toCodepoint(physKey, outEv.modifiers);
                    if (m_callback) m_callback(outEv);
                    count++;
                }
            }
        }
    }
    return count;
}

void EvdevInputBackend::shutdown() {
    if (!m_initialized && !m_udev && !m_libinput && m_evdevDevices.empty()) return;
    std::cout << "[LCL Input] Shutting down Input Subsystem...\n";
    cleanup();
    m_initialized = false;
}

void EvdevInputBackend::cleanup() {
    for (auto& d : m_evdevDevices) {
        if (d.fd >= 0) {
            close(d.fd);
            d.fd = -1;
        }
    }
    m_evdevDevices.clear();

    if (m_netlinkFd >= 0) {
        close(m_netlinkFd);
        m_netlinkFd = -1;
    }

    if (m_libinput) {
        libinput_unref(m_libinput);
        m_libinput = nullptr;
    }
    if (m_udev) {
        udev_unref(m_udev);
        m_udev = nullptr;
    }
}

uint8_t EvdevInputBackend::getActiveModifiers() const {
    uint8_t mods = 0;
    if (m_shiftPressed) mods |= kModShift;
    if (m_ctrlPressed) mods |= kModCtrl;
    if (m_altPressed) mods |= kModAlt;
    if (m_capsLockActive) mods |= kModCapsLock;
    if (m_superPressed) mods |= kModSuper;
    return mods;
}

} // namespace lcl::platform::desktop
