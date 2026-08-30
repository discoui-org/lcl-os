#include "platforms/android/android_input_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace lcl::platform::android {

AndroidInputBackend::AndroidInputBackend() = default;

AndroidInputBackend::~AndroidInputBackend() {
    shutdown();
}

AndroidInputBackend::AndroidInputBackend(AndroidInputBackend&& other) noexcept
    : m_devices(std::move(other.m_devices)),
      m_callback(std::move(other.m_callback)),
      m_initialized(other.m_initialized) {
    other.m_initialized = false;
}

AndroidInputBackend& AndroidInputBackend::operator=(AndroidInputBackend&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_devices = std::move(other.m_devices);
        m_callback = std::move(other.m_callback);
        m_initialized = other.m_initialized;
        other.m_initialized = false;
    }
    return *this;
}

bool AndroidInputBackend::initialize(lcl::platform::InputEventCallback callback) {
    if (callback) {
        m_callback = std::move(callback);
    }
    if (m_initialized) return true;
    std::cout << "[LCL Android Input] Initializing Android input subsystem...\n";

    scanInputDevices();
    m_initialized = true;
    std::cout << "[LCL Android Input] Backend active with " << m_devices.size() << " input device(s).\n";
    return true;
}

void AndroidInputBackend::shutdown() {
    if (!m_initialized && m_devices.empty()) return;
    std::cout << "[LCL Android Input] Shutting down Android input backend...\n";
    cleanup();
    m_initialized = false;
    m_callback = nullptr;
}

void AndroidInputBackend::cleanup() {
    for (auto& dev : m_devices) {
        if (dev.fd >= 0) {
            close(dev.fd);
            dev.fd = -1;
        }
    }
    m_devices.clear();
}

size_t AndroidInputBackend::scanInputDevices() {
    const std::string inputDir = "/dev/input";
    DIR* dir = opendir(inputDir.c_str());
    if (!dir) {
        std::cerr << "[LCL Android Input] Warning: Unable to open " << inputDir << " (" << std::strerror(errno) << ")\n";
        return 0;
    }

    size_t discovered = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.rfind("event", 0) != 0) continue;

        std::string path = inputDir + "/" + name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        Device dev{};
        dev.fd = fd;
        dev.path = path;

        char devName[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(devName)), devName) >= 0) {
            dev.name = devName;
        }

        // Check property bits
        uint8_t propBits[INPUT_PROP_MAX / 8 + 1] = {};
        bool hasPropDirect = false;
        bool hasPropPointer = false;
        if (ioctl(fd, EVIOCGPROP(sizeof(propBits)), propBits) >= 0) {
            hasPropDirect = (propBits[INPUT_PROP_DIRECT / 8] >> (INPUT_PROP_DIRECT % 8)) & 1;
            hasPropPointer = (propBits[INPUT_PROP_POINTER / 8] >> (INPUT_PROP_POINTER % 8)) & 1;
        }

        // Check EV_ABS capabilities
        uint8_t absBits[KEY_MAX / 8 + 1] = {};
        bool hasMTTrackingId = false;
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0) {
            dev.hasAbsX = (absBits[ABS_X / 8] >> (ABS_X % 8)) & 1;
            dev.hasAbsY = (absBits[ABS_Y / 8] >> (ABS_Y % 8)) & 1;
            dev.hasMTAbsX = (absBits[ABS_MT_POSITION_X / 8] >> (ABS_MT_POSITION_X % 8)) & 1;
            dev.hasMTAbsY = (absBits[ABS_MT_POSITION_Y / 8] >> (ABS_MT_POSITION_Y % 8)) & 1;
            hasMTTrackingId = (absBits[ABS_MT_TRACKING_ID / 8] >> (ABS_MT_TRACKING_ID % 8)) & 1;

            int axisX = dev.hasMTAbsX ? ABS_MT_POSITION_X : (dev.hasAbsX ? ABS_X : -1);
            int axisY = dev.hasMTAbsY ? ABS_MT_POSITION_Y : (dev.hasAbsY ? ABS_Y : -1);

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
        }

        // Direct touchscreen identification rule:
        // 1) Multi-touch direct touchscreen: ABS_MT_POSITION_X + ABS_MT_POSITION_Y + ABS_MT_TRACKING_ID without INPUT_PROP_POINTER
        // 2) Explicit direct property: (hasMTAbs or hasAbs) + INPUT_PROP_DIRECT
        // 3) Single-touch absolute screen: ABS_X + ABS_Y + (INPUT_PROP_DIRECT or without INPUT_PROP_POINTER)
        const bool isMTTouchscreen = (dev.hasMTAbsX && dev.hasMTAbsY && hasMTTrackingId && !hasPropPointer);
        const bool isExplicitDirect = (hasPropDirect && (dev.hasMTAbsX || dev.hasMTAbsY || (dev.hasAbsX && dev.hasAbsY)));
        const bool isSingleTouchscreen = (dev.hasAbsX && dev.hasAbsY && !hasPropPointer && !dev.hasMTAbsX);

        if (isMTTouchscreen || isExplicitDirect || isSingleTouchscreen) {
            dev.isDirectTouchscreen = true;
        }

        std::cout << "[LCL Android Input] Device: " << dev.path << " (" << dev.name << ") "
                  << (dev.isDirectTouchscreen ? "[DIRECT TOUCHSCREEN]" : "[OTHER]")
                  << " X:[" << dev.absXMin << ".." << dev.absXMax << "] Y:[" << dev.absYMin << ".." << dev.absYMax << "]\n";

        m_devices.push_back(dev);
        discovered++;
    }
    closedir(dir);
    return discovered;
}

size_t AndroidInputBackend::pollEvents(int screenWidth, int screenHeight) {
    if (!m_initialized || m_devices.empty()) {
        if (m_devices.empty()) scanInputDevices();
        if (m_devices.empty()) return 0;
    }

    std::vector<struct pollfd> fds;
    fds.reserve(m_devices.size());
    for (const auto& dev : m_devices) {
        fds.push_back({dev.fd, POLLIN, 0});
    }

    int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), 0);
    if (ready <= 0) return 0;

    size_t count = 0;
    for (size_t i = 0; i < m_devices.size(); ++i) {
        if (!(fds[i].revents & POLLIN)) continue;

        auto& dev = m_devices[i];
        struct input_event ev{};

        while (read(dev.fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                    dev.currentAbsX = ev.value;
                    dev.absXUpdated = true;
                } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                    dev.currentAbsY = ev.value;
                    dev.absYUpdated = true;
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0) {
                        dev.isTouching = true;
                        dev.touchPressed = true;
                    } else {
                        dev.isTouching = false;
                        dev.touchReleased = true;
                    }
                }
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_TOUCH) {
                    if (ev.value != 0) {
                        dev.isTouching = true;
                        dev.touchPressed = true;
                    } else {
                        dev.isTouching = false;
                        dev.touchReleased = true;
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
                        normX = dev.lastTouchX;
                    }

                    if (rangeY > 0.0 && dev.currentAbsY >= 0) {
                        normY = (static_cast<double>(dev.currentAbsY - dev.absYMin) / rangeY) * screenHeight;
                        normY = std::clamp(normY, 0.0, static_cast<double>(screenHeight - 1));
                    } else {
                        normY = dev.lastTouchY;
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
                        if (m_callback) m_callback(downEv);

                        dev.lastTouchX = normX;
                        dev.lastTouchY = normY;
                        dev.touchPressed = false;
                        count++;
                    } else if (dev.touchReleased) {
                        RawInputEvent upEv{};
                        upEv.type = RawInputEventType::PointerButton;
                        upEv.source = PointerSource::Touch;
                        upEv.button = PointerButton::Left;
                        upEv.pressed = false;
                        upEv.absoluteX = normX >= 0.0 ? normX : dev.lastTouchX;
                        upEv.absoluteY = normY >= 0.0 ? normY : dev.lastTouchY;
                        upEv.timestampNs = timestampNs;
                        upEv.deviceName = dev.name;
                        if (m_callback) m_callback(upEv);

                        dev.lastTouchX = -1.0;
                        dev.lastTouchY = -1.0;
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
                            if (m_callback) m_callback(moveEv);

                            dev.lastTouchX = normX;
                            dev.lastTouchY = normY;
                            count++;
                        }
                    }

                    dev.absXUpdated = false;
                    dev.absYUpdated = false;
                }
            }
        }
    }

    return count;
}

} // namespace lcl::platform::android
