#include "platform/android/android_hidl_display_backend.hpp"
#include "platform/android/android_hidl_bridge.h"

#include <android/hardware_buffer.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <unistd.h>

#if defined(LCL_HAS_ANDROID_HIDL)

#ifndef PAGE_SIZE
#define PAGE_SIZE getpagesize()
#endif

#define LOG_TAG "LclHidlDisplay"
#include <android/hardware/graphics/composer/2.2/IComposer.h>
#include <android/hardware/graphics/composer/2.2/IComposerClient.h>
#include <android/hardware/graphics/composer/2.4/IComposer.h>
#include <android/hardware/graphics/composer/2.4/IComposerClient.h>
#include <android/hardware/graphics/composer/2.1/IComposerCallback.h>
#include <composer-command-buffer/2.4/ComposerCommandBuffer.h>
#include <hidl/HidlTransportSupport.h>
#include <sync/sync.h>
#include <unistd.h>

#include <condition_variable>
#include <array>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <vector>

namespace composer21 = ::android::hardware::graphics::composer::V2_1;
namespace composer22 = ::android::hardware::graphics::composer::V2_2;
namespace composer24 = ::android::hardware::graphics::composer::V2_4;
namespace common10 = ::android::hardware::graphics::common::V1_0;
namespace common12 = ::android::hardware::graphics::common::V1_2;

extern "C" const native_handle_t* AHardwareBuffer_getNativeHandle(
    const AHardwareBuffer* buffer);

#endif

namespace lcl::platform::android {

#if defined(LCL_HAS_ANDROID_HIDL)

using ::android::hardware::Return;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_vec;
using ::android::sp;

class HidlCommandReader final : public composer24::CommandReaderBase {
public:
    bool parse() {
        composer21::IComposerClient::Command command{};
        uint16_t length = 0;
        while (!isEmpty()) {
            if (!beginCommand(&command, &length)) return false;
            bool parsed = true;
            switch (command) {
            case composer21::IComposerClient::Command::SELECT_DISPLAY:
                parsed = length == composer21::CommandWriterBase::kSelectDisplayLength;
                if (parsed) m_currentDisplay = read64();
                break;
            case composer21::IComposerClient::Command::SET_ERROR:
                parsed = length == composer21::CommandWriterBase::kSetErrorLength;
                if (parsed) {
                    const uint32_t location = read();
                    const auto error = static_cast<composer21::Error>(readSigned());
                    m_errors.emplace_back(location, error);
                }
                break;
            case composer21::IComposerClient::Command::SET_CHANGED_COMPOSITION_TYPES:
                parsed = length % 3 == 0;
                while (parsed && length > 0) {
                    const uint64_t layer = read64();
                    const auto type = static_cast<composer21::IComposerClient::Composition>(
                        readSigned());
                    m_changedTypes.emplace_back(layer, type);
                    length -= 3;
                }
                break;
            case composer21::IComposerClient::Command::SET_DISPLAY_REQUESTS:
                parsed = length % 3 == 1;
                if (parsed) {
                    m_displayRequestMask = read();
                    while (length > 1) {
                        (void)read64();
                        (void)read();
                        length -= 3;
                    }
                }
                break;
            case composer21::IComposerClient::Command::SET_PRESENT_FENCE:
                parsed = length == composer21::CommandWriterBase::kSetPresentFenceLength;
                if (parsed) {
                    if (m_presentFence >= 0) close(m_presentFence);
                    m_presentFence = readFence();
                }
                break;
            case composer21::IComposerClient::Command::SET_RELEASE_FENCES:
                parsed = length % 3 == 0;
                while (parsed && length > 0) {
                    const uint64_t layer = read64();
                    const int fence = readFence();
                    m_releaseFences.emplace_back(layer, fence);
                    length -= 3;
                }
                break;
            case composer21::IComposerClient::Command::SET_PRESENT_OR_VALIDATE_DISPLAY_RESULT:
                parsed = length == composer21::CommandWriterBase::kPresentOrValidateDisplayResultLength;
                if (parsed) m_presentOrValidateState = static_cast<int32_t>(read());
                break;
            case static_cast<composer21::IComposerClient::Command>(
                composer24::IComposerClient::Command::SET_CLIENT_TARGET_PROPERTY):
                parsed = length == 2;
                if (parsed) {
                    (void)readSigned();
                    (void)readSigned();
                }
                break;
            default:
                parsed = false;
                break;
            }
            endCommand();
            if (!parsed) return false;
        }
        return true;
    }

    void resetResults() {
        m_errors.clear();
        m_changedTypes.clear();
        m_displayRequestMask = 0;
        m_presentOrValidateState = -1;
        m_currentDisplay = 0;
        if (m_presentFence >= 0) {
            close(m_presentFence);
            m_presentFence = -1;
        }
        for (const auto& [layer, fence] : m_releaseFences) {
            (void)layer;
            if (fence >= 0) close(fence);
        }
        m_releaseFences.clear();
    }

    bool hasChangedTypes() const { return !m_changedTypes.empty(); }
    bool changedToClient(uint64_t layer) const {
        for (const auto& [changedLayer, type] : m_changedTypes) {
            if (changedLayer == layer && type == composer21::IComposerClient::Composition::CLIENT) {
                return true;
            }
        }
        return false;
    }
    uint32_t displayRequestMask() const { return m_displayRequestMask; }
    bool presentOrValidatePresented() const { return m_presentOrValidateState == 1; }
    bool hasErrors() const { return !m_errors.empty(); }
    int takePresentFence() {
        const int fence = m_presentFence;
        m_presentFence = -1;
        return fence;
    }
    std::vector<std::pair<uint64_t, int>> takeReleaseFences() {
        return std::move(m_releaseFences);
    }
    const std::vector<std::pair<uint32_t, composer21::Error>>& errors() const { return m_errors; }

private:
    uint64_t m_currentDisplay{0};
    uint32_t m_displayRequestMask{0};
    int32_t m_presentOrValidateState{-1};
    int m_presentFence{-1};
    std::vector<std::pair<uint64_t, composer21::IComposerClient::Composition>> m_changedTypes;
    std::vector<std::pair<uint64_t, int>> m_releaseFences;
    std::vector<std::pair<uint32_t, composer21::Error>> m_errors;
};

struct AndroidHidlDisplayBackend::Impl {
    sp<composer24::IComposer> composer24;
    sp<composer22::IComposer> composer22;
    sp<composer22::IComposerClient> client;
    sp<composer21::IComposerCallback> callback;
    composer24::CommandWriterBase writer{1024};
    HidlCommandReader reader;
    std::mutex mutex;
    std::condition_variable cv;
    bool hotplugReceived{false};
    uint64_t hotplugDisplayId{0};
    bool hotplugConnected{false};
    uint32_t composerMinorVersion{0};
    std::array<const AHardwareBuffer*, 4> layerBufferSlots{};
    std::array<std::vector<int>, 4> pendingSlotFences{};
    uint32_t nextLayerBufferSlot{0};
};

static void waitAndClearFences(std::vector<int>& fences) {
    for (const int fence : fences) {
        if (fence >= 0) {
            (void)sync_wait(fence, 1000);
            close(fence);
        }
    }
    fences.clear();
}

class HidlComposerCallback final : public composer21::IComposerCallback {
public:
    explicit HidlComposerCallback(AndroidHidlDisplayBackend::Impl* impl) : m_impl(impl) {}

    Return<void> onHotplug(uint64_t display, Connection connection) override {
        if (m_impl) {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->hotplugDisplayId = display;
            m_impl->hotplugConnected = connection == Connection::CONNECTED;
            m_impl->hotplugReceived = true;
            m_impl->cv.notify_all();
        }
        return {};
    }
    Return<void> onRefresh(uint64_t) override { return {}; }
    Return<void> onVsync(uint64_t, int64_t) override { return {}; }

private:
    AndroidHidlDisplayBackend::Impl* m_impl;
};

static bool isNone(composer21::Error error) {
    return error == composer21::Error::NONE;
}

#else

struct AndroidHidlDisplayBackend::Impl {};

#endif

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
}

bool AndroidHidlDisplayBackend::initialize(float outputScale,
                                           uint32_t preferredWidth,
                                           uint32_t preferredHeight,
                                           uint32_t preferredRefreshHz) {
#if !defined(LCL_HAS_ANDROID_HIDL)
    (void)outputScale;
    (void)preferredWidth;
    (void)preferredHeight;
    (void)preferredRefreshHz;
    std::cerr << "[AndroidHidlDisplayBackend] HIDL support was not enabled at build time\n";
    return false;
#else
    if (m_initialized) return true;
    m_activeMode.scaleFactor = outputScale;
    m_impl->hotplugReceived = false;
    m_impl->hotplugDisplayId = 0;
    m_impl->hotplugConnected = false;

    ::android::hardware::configureRpcThreadpool(1, false);
    composer21::Error createError = composer21::Error::NO_RESOURCES;
    m_impl->composer24 = composer24::IComposer::tryGetService("default", false);
    if (m_impl->composer24) {
        for (int retry = 0; retry < 15 && !m_impl->client; ++retry) {
            const auto result = m_impl->composer24->createClient_2_4(
                [&](composer24::Error error, const sp<composer24::IComposerClient>& client) {
                    createError = static_cast<composer21::Error>(error);
                    m_impl->client = client;
                });
            if (!result.isOk()) {
                std::cerr << "[AndroidHidlDisplayBackend] createClient_2_4 transaction failed: "
                          << result.description() << "\n";
            }
            if (!m_impl->client) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (m_impl->client && createError == composer21::Error::NONE) {
            m_impl->composerMinorVersion = 4;
        }
    }

    if (!m_impl->client) {
        createError = composer21::Error::NO_RESOURCES;
        m_impl->composer22 = composer22::IComposer::tryGetService("default", false);
        if (m_impl->composer22) {
            for (int retry = 0; retry < 15 && !m_impl->client; ++retry) {
                const auto result = m_impl->composer22->createClient(
                    [&](composer21::Error error,
                        const sp<composer21::IComposerClient>& client21) {
                        createError = error;
                        if (!client21 || error != composer21::Error::NONE) return;
                        const auto castResult = composer22::IComposerClient::castFrom(client21);
                        if (!castResult.isOk()) {
                            std::cerr << "[AndroidHidlDisplayBackend] Composer 2.2 client cast failed: "
                                      << castResult.description() << "\n";
                            return;
                        }
                        m_impl->client = castResult;
                    });
                if (!result.isOk()) {
                    std::cerr << "[AndroidHidlDisplayBackend] createClient 2.2 transaction failed: "
                              << result.description() << "\n";
                }
                if (!m_impl->client) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (m_impl->client && createError == composer21::Error::NONE) {
                m_impl->composerMinorVersion = 2;
            }
        }
    }

    if (!m_impl->client || createError != composer21::Error::NONE) {
        std::cerr << "[AndroidHidlDisplayBackend] Composer 2.4/2.2 client creation failed "
                     "(likely owned by SurfaceFlinger): "
                  << composer21::toString(createError) << "\n";
        shutdown();
        return false;
    }

    m_impl->callback = new HidlComposerCallback(m_impl.get());
    const auto callbackResult = m_impl->client->registerCallback(m_impl->callback);
    if (!callbackResult.isOk()) {
        std::cerr << "[AndroidHidlDisplayBackend] registerCallback transaction failed: "
                  << callbackResult.description() << "\n";
        shutdown();
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        m_impl->cv.wait_for(lock, std::chrono::seconds(2), [this] {
            return m_impl->hotplugReceived;
        });
    }
    if (!m_impl->hotplugReceived || !m_impl->hotplugConnected) {
        std::cerr << "[AndroidHidlDisplayBackend] no connected display hotplug callback\n";
        shutdown();
        return false;
    }
    m_displayId = m_impl->hotplugDisplayId;
    m_displayConnected = true;

    // Client targets have a display-scoped buffer cache separate from layer
    // buffers. Reserve the slots before any SET_CLIENT_TARGET command; older
    // Composer 2.2 services reject otherwise valid handles as BAD_PARAMETER.
    const auto clientTargetSlotsResult =
        m_impl->client->setClientTargetSlotCount(m_displayId, 4);
    if (!clientTargetSlotsResult.isOk() || !isNone(clientTargetSlotsResult)) {
        std::cerr << "[AndroidHidlDisplayBackend] setClientTargetSlotCount failed\n";
        shutdown();
        return false;
    }

    uint32_t activeConfig = 0;
    composer21::Error queryError = composer21::Error::NO_RESOURCES;
    m_impl->client->getActiveConfig(m_displayId, [&](composer21::Error error, uint32_t config) {
        queryError = error;
        activeConfig = config;
    });
    if (!isNone(queryError)) {
        std::cerr << "[AndroidHidlDisplayBackend] getActiveConfig failed: "
                  << composer21::toString(queryError) << "\n";
        shutdown();
        return false;
    }

    const auto readAttribute = [&](uint32_t config,
                                   composer21::IComposerClient::Attribute attribute,
                                   int32_t* destination) {
        composer21::Error error = composer21::Error::NO_RESOURCES;
        m_impl->client->getDisplayAttribute(
            m_displayId, config, attribute,
            [&](composer21::Error callbackError, int32_t value) {
                error = callbackError;
                if (isNone(error)) *destination = value;
            });
        return error;
    };

    if (preferredWidth > 0 && preferredHeight > 0) {
        composer21::Error configsError = composer21::Error::NO_RESOURCES;
        hidl_vec<uint32_t> configs;
        m_impl->client->getDisplayConfigs(
            m_displayId, [&](composer21::Error error, const hidl_vec<uint32_t>& values) {
                configsError = error;
                if (isNone(error)) configs = values;
            });
        bool foundPreferred = false;
        uint32_t preferredConfig = activeConfig;
        int64_t bestRefreshDistance = std::numeric_limits<int64_t>::max();
        if (isNone(configsError)) {
            for (const uint32_t config : configs) {
                int32_t configWidth = 0;
                int32_t configHeight = 0;
                int32_t configVsyncPeriod = 0;
                if (!isNone(readAttribute(config, composer21::IComposerClient::Attribute::WIDTH,
                                          &configWidth)) ||
                    !isNone(readAttribute(config, composer21::IComposerClient::Attribute::HEIGHT,
                                          &configHeight)) ||
                    configWidth != static_cast<int32_t>(preferredWidth) ||
                    configHeight != static_cast<int32_t>(preferredHeight)) {
                    continue;
                }
                (void)readAttribute(config,
                                    composer21::IComposerClient::Attribute::VSYNC_PERIOD,
                                    &configVsyncPeriod);
                const int64_t refreshHz = configVsyncPeriod > 0
                    ? 1000000000ll / configVsyncPeriod : 0;
                const int64_t distance = preferredRefreshHz > 0
                    ? std::llabs(refreshHz - preferredRefreshHz) : 0;
                if (!foundPreferred || distance < bestRefreshDistance) {
                    foundPreferred = true;
                    preferredConfig = config;
                    bestRefreshDistance = distance;
                }
            }
        }
        if (!foundPreferred) {
            std::cerr << "[AndroidHidlDisplayBackend] Gestalt mode "
                      << preferredWidth << "x" << preferredHeight
                      << " is unavailable; using Composer active mode\n";
        } else if (preferredConfig != activeConfig) {
            const auto setResult = m_impl->client->setActiveConfig(m_displayId, preferredConfig);
            if (setResult.isOk() && isNone(setResult)) {
                activeConfig = preferredConfig;
            } else {
                std::cerr << "[AndroidHidlDisplayBackend] Composer rejected Gestalt mode\n";
            }
        }
    }

    int32_t width = 0;
    int32_t height = 0;
    int32_t vsyncPeriod = 0;
    if (!isNone(readAttribute(activeConfig, composer21::IComposerClient::Attribute::WIDTH,
                              &width)) ||
        !isNone(readAttribute(activeConfig, composer21::IComposerClient::Attribute::HEIGHT,
                              &height))) {
        std::cerr << "[AndroidHidlDisplayBackend] failed to query display dimensions\n";
        shutdown();
        return false;
    }
    (void)readAttribute(activeConfig, composer21::IComposerClient::Attribute::VSYNC_PERIOD,
                        &vsyncPeriod);
    m_activeMode.width = width;
    m_activeMode.height = height;
    if (vsyncPeriod > 0) {
        m_activeMode.refreshRateHz = 1000000000 / vsyncPeriod;
        m_activeMode.refreshRate = m_activeMode.refreshRateHz;
    }
    m_activeMode.name = "Android HIDL Display " + std::to_string(m_displayId) + " (" +
                        std::to_string(width) + "x" + std::to_string(height) + ")";

    const auto powerResult = m_impl->client->setPowerMode(
        m_displayId, composer21::IComposerClient::PowerMode::ON);
    if (!powerResult.isOk() || !isNone(powerResult)) {
        std::cerr << "[AndroidHidlDisplayBackend] setPowerMode(ON) failed\n";
    }

    composer21::Error layerError = composer21::Error::NO_RESOURCES;
    m_impl->client->createLayer(
        m_displayId, 4,
        [&](composer21::Error error, uint64_t layer) {
            layerError = error;
            m_layerId = layer;
        });
    if (!isNone(layerError)) {
        std::cerr << "[AndroidHidlDisplayBackend] createLayer failed: "
                  << composer21::toString(layerError) << "\n";
        shutdown();
        return false;
    }
    m_hasLayer = true;
    m_initialized = true;
    std::cerr << "[AndroidHidlDisplayBackend] initialized Composer 2."
              << m_impl->composerMinorVersion << " on display "
              << m_displayId << " at " << width << "x" << height << "\n";
    return true;
#endif
}

void AndroidHidlDisplayBackend::shutdown() {
#if defined(LCL_HAS_ANDROID_HIDL)
    // Keep scanout buffers alive until Composer has released every slot. The
    // platform service shuts this backend down before destroying its AHBs.
    for (auto& fences : m_impl->pendingSlotFences) {
        waitAndClearFences(fences);
    }
    if (m_hasLayer && m_impl->client) {
        (void)m_impl->client->destroyLayer(m_displayId, m_layerId);
    }
    m_impl->reader.resetResults();
    m_impl->writer.reset();
    m_impl->callback.clear();
    m_impl->client.clear();
    m_impl->composer24.clear();
    m_impl->composer22.clear();
    m_impl->hotplugReceived = false;
    m_impl->hotplugDisplayId = 0;
    m_impl->hotplugConnected = false;
    m_impl->composerMinorVersion = 0;
    m_impl->layerBufferSlots.fill(nullptr);
    for (auto& fences : m_impl->pendingSlotFences) fences.clear();
    m_impl->nextLayerBufferSlot = 0;
#endif
    m_initialized = false;
    m_displayConnected = false;
    m_hasLayer = false;
    m_layerId = 0;
}

bool AndroidHidlDisplayBackend::presentBuffer(AHardwareBuffer* buffer, int acquireFenceFd) {
#if !defined(LCL_HAS_ANDROID_HIDL)
    (void)buffer;
    (void)acquireFenceFd;
    return false;
#else
    if (!m_initialized || !m_impl->client || !m_hasLayer || !buffer) return false;
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(buffer);
    if (!handle) return false;

    using Client = composer21::IComposerClient;
    const int32_t width = m_activeMode.width;
    const int32_t height = m_activeMode.height;
    const Client::Rect frame{0, 0, width, height};
    const Client::FRect crop{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    const std::vector<Client::Rect> region{frame};

    // Composer buffer handles are cached by slot. Keep each scanout AHB on a
    // stable slot instead of replacing slot 0 with a different handle every frame.
    uint32_t bufferSlot = 0;
    bool knownBuffer = false;
    for (uint32_t slot = 0; slot < m_impl->layerBufferSlots.size(); ++slot) {
        if (m_impl->layerBufferSlots[slot] == buffer) {
            bufferSlot = slot;
            knownBuffer = true;
            break;
        }
    }
    if (!knownBuffer) {
        std::cerr << "[AndroidHidlDisplayBackend] scanout buffer was not prepared before present\n";
        return false;
    }

    auto execute = [&]() -> bool {
        bool inputQueueChanged = false;
        uint32_t commandLength = 0;
        hidl_vec<hidl_handle> commandHandles;
        if (!m_impl->writer.writeQueue(&inputQueueChanged, &commandLength, &commandHandles)) {
            return false;
        }
        if (inputQueueChanged) {
            const auto queueError = m_impl->client->setInputCommandQueue(
                *m_impl->writer.getMQDescriptor());
            if (!queueError.isOk() || !isNone(queueError)) return false;
        }

        composer21::Error executeError = composer21::Error::NO_RESOURCES;
        const auto result = m_impl->client->executeCommands_2_2(
            commandLength, commandHandles,
            [&](composer21::Error error, bool outputQueueChanged, uint32_t outputLength,
                const hidl_vec<hidl_handle>& outputHandles) {
                executeError = error;
                if (!isNone(executeError)) return;
                if (outputQueueChanged) {
                    m_impl->client->getOutputCommandQueue(
                        [&](composer21::Error queueError, const auto& descriptor) {
                            executeError = queueError;
                            if (isNone(queueError) && !m_impl->reader.setMQDescriptor(descriptor)) {
                                executeError = composer21::Error::NO_RESOURCES;
                            }
                        });
                }
                if (isNone(executeError) &&
                    (!m_impl->reader.readQueue(outputLength, outputHandles) ||
                     !m_impl->reader.parse())) {
                    executeError = composer21::Error::NO_RESOURCES;
                }
            });
        m_impl->writer.reset();
        if (!result.isOk() || !isNone(executeError)) return false;
        for (const auto& [location, error] : m_impl->reader.errors()) {
            std::cerr << "[AndroidHidlDisplayBackend] command error at " << location
                      << ": " << composer21::toString(error) << "\n";
        }
        return !m_impl->reader.hasErrors();
    };

    m_impl->reader.resetResults();
    m_impl->writer.selectDisplay(m_displayId);
    m_impl->writer.selectLayer(m_layerId);
    m_impl->writer.setLayerBuffer(bufferSlot, handle,
                                  acquireFenceFd >= 0 ? dup(acquireFenceFd) : -1);
    m_impl->writer.setLayerSurfaceDamage(region);
    m_impl->writer.setLayerBlendMode(Client::BlendMode::NONE);
    m_impl->writer.setLayerCompositionType(Client::Composition::DEVICE);
    m_impl->writer.setLayerDataspace(common12::Dataspace::UNKNOWN);
    m_impl->writer.setLayerDisplayFrame(frame);
    m_impl->writer.setLayerPlaneAlpha(1.0f);
    m_impl->writer.setLayerSourceCrop(crop);
    m_impl->writer.setLayerTransform(static_cast<common10::Transform>(0));
    m_impl->writer.setLayerVisibleRegion(region);
    m_impl->writer.setLayerZOrder(0);
    // Composer 2.2+ can present immediately when the established composition
    // remains valid. This collapses the common move-frame path from separate
    // validate and present HIDL transactions into one executeCommands call.
    m_impl->writer.presentOrvalidateDisplay();
    if (!execute()) return false;

    const auto retainCompletionFences = [&] {
        const int presentFence = m_impl->reader.takePresentFence();
        auto releaseFences = m_impl->reader.takeReleaseFences();
        if (presentFence >= 0) {
            m_impl->pendingSlotFences[bufferSlot].push_back(presentFence);
        }
        for (const auto& [layer, fence] : releaseFences) {
            (void)layer;
            if (fence >= 0) {
                m_impl->pendingSlotFences[bufferSlot].push_back(fence);
            }
        }
    };

    if (m_impl->reader.presentOrValidatePresented()) {
        retainCompletionFences();
        return true;
    }

    const bool changedTypes = m_impl->reader.hasChangedTypes();
    const bool needsClientTarget =
        m_impl->reader.changedToClient(m_layerId) ||
        (m_impl->reader.displayRequestMask() &
         static_cast<uint32_t>(Client::DisplayRequest::FLIP_CLIENT_TARGET)) != 0;
    m_impl->reader.resetResults();
    m_impl->writer.selectDisplay(m_displayId);
    if (changedTypes) m_impl->writer.acceptDisplayChanges();
    if (needsClientTarget) {
        // LCL has already composited the full display into this buffer. If the
        // HAL rejects DEVICE composition, that frame is the required client target.
        m_impl->writer.setClientTarget(bufferSlot, handle,
                                       acquireFenceFd >= 0 ? dup(acquireFenceFd) : -1,
                                       common12::Dataspace::UNKNOWN, region);
    }
    m_impl->writer.presentDisplay();
    if (!execute()) return false;

    // A present fence does not replace the per-layer release fence. Retain
    // both, but wait only when this AHB slot rotates back to the renderer.
    retainCompletionFences();
    return true;
#endif
}

bool AndroidHidlDisplayBackend::prepareBufferForRender(AHardwareBuffer* buffer) {
#if !defined(LCL_HAS_ANDROID_HIDL)
    (void)buffer;
    return false;
#else
    if (!m_initialized || !m_impl->client || !m_hasLayer || !buffer) return false;

    uint32_t bufferSlot = 0;
    bool knownBuffer = false;
    for (uint32_t slot = 0; slot < m_impl->layerBufferSlots.size(); ++slot) {
        if (m_impl->layerBufferSlots[slot] == buffer) {
            bufferSlot = slot;
            knownBuffer = true;
            break;
        }
    }
    if (!knownBuffer) {
        bufferSlot = m_impl->nextLayerBufferSlot;
        waitAndClearFences(m_impl->pendingSlotFences[bufferSlot]);
        m_impl->layerBufferSlots[bufferSlot] = buffer;
        m_impl->nextLayerBufferSlot =
            (m_impl->nextLayerBufferSlot + 1) % m_impl->layerBufferSlots.size();
    } else {
        waitAndClearFences(m_impl->pendingSlotFences[bufferSlot]);
    }
    return true;
#endif
}

} // namespace lcl::platform::android

#define LCL_HIDL_BRIDGE_EXPORT __attribute__((visibility("default")))

extern "C" LCL_HIDL_BRIDGE_EXPORT void* lcl_android_hidl_create(void) {
    return new lcl::platform::android::AndroidHidlDisplayBackend();
}

extern "C" LCL_HIDL_BRIDGE_EXPORT void lcl_android_hidl_destroy(void* instance) {
    delete static_cast<lcl::platform::android::AndroidHidlDisplayBackend*>(instance);
}

extern "C" LCL_HIDL_BRIDGE_EXPORT int lcl_android_hidl_initialize(
    void* instance, float outputScale, uint32_t preferredWidth,
    uint32_t preferredHeight, uint32_t preferredRefreshHz,
    LclAndroidHidlDisplayInfo* info) {
    auto* backend = static_cast<lcl::platform::android::AndroidHidlDisplayBackend*>(instance);
    if (!backend || !info ||
        !backend->initialize(outputScale, preferredWidth, preferredHeight,
                             preferredRefreshHz)) return 0;
    const auto& mode = backend->activeMode();
    info->width = mode.width;
    info->height = mode.height;
    info->refresh_rate_hz = mode.refreshRateHz;
    info->scale_factor = mode.scaleFactor;
    info->display_id = backend->displayId();
    info->layer_id = backend->layerId();
    info->connected = backend->isDisplayConnected() ? 1 : 0;
    snprintf(info->name, sizeof(info->name), "%s", mode.name.c_str());
    return 1;
}

extern "C" LCL_HIDL_BRIDGE_EXPORT void lcl_android_hidl_shutdown(void* instance) {
    auto* backend = static_cast<lcl::platform::android::AndroidHidlDisplayBackend*>(instance);
    if (backend) backend->shutdown();
}

extern "C" LCL_HIDL_BRIDGE_EXPORT int lcl_android_hidl_prepare_buffer(
    void* instance, AHardwareBuffer* buffer) {
    auto* backend = static_cast<lcl::platform::android::AndroidHidlDisplayBackend*>(instance);
    return backend && backend->prepareBufferForRender(buffer) ? 1 : 0;
}

extern "C" LCL_HIDL_BRIDGE_EXPORT int lcl_android_hidl_present(
    void* instance, AHardwareBuffer* buffer, int acquireFenceFd) {
    auto* backend = static_cast<lcl::platform::android::AndroidHidlDisplayBackend*>(instance);
    return backend && backend->presentBuffer(buffer, acquireFenceFd) ? 1 : 0;
}
