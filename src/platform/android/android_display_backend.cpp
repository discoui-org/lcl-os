#include "platform/android/android_display_backend.hpp"
#include "platform/android/android_hidl_display_backend.hpp"
#include "platform/common/output_scale.hpp"

#include <aidl/android/hardware/graphics/composer3/IComposer.h>
#include <aidl/android/hardware/graphics/composer3/IComposerClient.h>
#include <aidl/android/hardware/graphics/composer3/BnComposerCallback.h>
#include <aidl/android/hardware/graphics/composer3/DisplayCommand.h>
#include <aidl/android/hardware/graphics/composer3/LayerCommand.h>
#include <aidl/android/hardware/graphics/composer3/Buffer.h>
#include <aidl/android/hardware/graphics/composer3/CommandResultPayload.h>
#include <aidl/android/hardware/graphics/composer3/PresentOrValidate.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableComposition.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableBlendMode.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableDataspace.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableTransform.h>
#include <aidl/android/hardware/graphics/composer3/PlaneAlpha.h>
#include <aidl/android/hardware/graphics/composer3/ZOrder.h>
#include <aidl/android/hardware/graphics/common/Rect.h>
#include <aidl/android/hardware/graphics/common/FRect.h>
#include <aidl/android/hardware/graphics/common/BlendMode.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/Transform.h>
#include <aidl/android/hardware/common/NativeHandle.h>

#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_parcel.h>
#include <android/hardware_buffer.h>

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <vector>

extern "C" {
    typedef struct native_handle {
        int version;
        int numFds;
        int numInts;
        int data[1];
    } native_handle_t;

    const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* buffer);

    AIBinder* AServiceManager_getService(const char* instance);
    void ABinderProcess_startThreadPool();
    void AIBinder_markVintfStability(AIBinder* binder);
}

namespace lcl::platform::android {

using aidl::android::hardware::graphics::composer3::IComposer;
using aidl::android::hardware::graphics::composer3::IComposerClient;
using aidl::android::hardware::graphics::composer3::BnComposerCallback;
using aidl::android::hardware::graphics::composer3::DisplayCommand;
using aidl::android::hardware::graphics::composer3::LayerCommand;
using aidl::android::hardware::graphics::composer3::Buffer;
using aidl::android::hardware::graphics::composer3::CommandResultPayload;
using aidl::android::hardware::graphics::composer3::PresentOrValidate;
using aidl::android::hardware::graphics::composer3::ParcelableComposition;
using aidl::android::hardware::graphics::composer3::Composition;
using aidl::android::hardware::graphics::composer3::ParcelableBlendMode;
using aidl::android::hardware::graphics::common::BlendMode;
using aidl::android::hardware::graphics::composer3::ParcelableDataspace;
using aidl::android::hardware::graphics::composer3::ParcelableTransform;
using aidl::android::hardware::graphics::composer3::PlaneAlpha;
using aidl::android::hardware::graphics::composer3::ZOrder;
using aidl::android::hardware::graphics::composer3::VsyncPeriodChangeTimeline;
using aidl::android::hardware::graphics::composer3::RefreshRateChangedDebugData;
using aidl::android::hardware::graphics::common::DisplayHotplugEvent;
using aidl::android::hardware::graphics::common::Dataspace;
using aidl::android::hardware::graphics::common::Transform;
using aidl::android::hardware::graphics::common::Rect;
using aidl::android::hardware::graphics::common::FRect;
using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::drm::HdcpLevels;

typedef AIBinder* (*pfn_AServiceManager_getService)(const char* instance);
typedef void (*pfn_ABinderProcess_startThreadPool)();
typedef void (*pfn_AIBinder_markVintfStability)(AIBinder* binder);

struct AndroidDisplayBackend::Impl {
    std::shared_ptr<IComposer> composer;
    std::shared_ptr<IComposerClient> client;
    std::shared_ptr<BnComposerCallback> callback;

    void* binderNdkLib{nullptr};
    pfn_AServiceManager_getService getService{nullptr};
    pfn_ABinderProcess_startThreadPool startThreadPool{nullptr};
    pfn_AIBinder_markVintfStability markVintfStability{nullptr};

    std::mutex mutex;
    std::condition_variable cv;
    bool hotplugReceived{false};
    int64_t hotplugDisplayId{-1};
    bool hotplugConnected{false};
    uint64_t vsyncSerial{0};
    bool vsyncEnabled{false};
    std::array<const AHardwareBuffer*, 4> layerBufferSlots{};
    std::array<bool, 4> layerBufferHandlesSent{};
    std::array<std::vector<int>, 4> pendingSlotFences{};
    uint32_t nextLayerBufferSlot{0};
};

static void waitAndClearFenceFds(std::vector<int>& fences) {
    for (const int fence : fences) {
        if (fence < 0) continue;
        pollfd descriptor{};
        descriptor.fd = fence;
        descriptor.events = POLLIN | POLLPRI;
        (void)poll(&descriptor, 1, 1000);
        close(fence);
    }
    fences.clear();
}

class AndroidComposerCallback final : public BnComposerCallback {
public:
    explicit AndroidComposerCallback(AndroidDisplayBackend::Impl* impl) : m_impl(impl) {}

    ::ndk::ScopedAStatus onHotplug(int64_t in_display, bool in_connected) override {
        if (m_impl) {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->hotplugDisplayId = in_display;
            m_impl->hotplugConnected = in_connected;
            m_impl->hotplugReceived = true;
            m_impl->cv.notify_all();
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onHotplugEvent(int64_t in_display, DisplayHotplugEvent in_event) override {
        bool connected = (in_event == DisplayHotplugEvent::CONNECTED);
        return onHotplug(in_display, connected);
    }

    ::ndk::ScopedAStatus onRefresh(int64_t /*in_display*/) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onSeamlessPossible(int64_t /*in_display*/) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onVsync(int64_t /*in_display*/, int64_t /*in_timestamp*/, int32_t /*in_vsyncPeriodNanos*/) override {
        if (m_impl) {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            ++m_impl->vsyncSerial;
            m_impl->cv.notify_all();
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onVsyncPeriodTimingChanged(int64_t /*in_display*/, const VsyncPeriodChangeTimeline& /*in_updatedTimeline*/) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onVsyncIdle(int64_t /*in_display*/) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onRefreshRateChangedDebug(const RefreshRateChangedDebugData& /*in_data*/) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onHdcpLevelsChanged(int64_t /*in_display*/, const HdcpLevels& /*in_levels*/) override {
        return ::ndk::ScopedAStatus::ok();
    }

private:
    AndroidDisplayBackend::Impl* m_impl{nullptr};
};

AndroidDisplayBackend::AndroidDisplayBackend()
    : m_impl(std::make_unique<Impl>()),
      m_hidlBackend(std::make_unique<AndroidHidlDisplayBackend>()) {
    m_activeMode.width = 320;
    m_activeMode.height = 640;
    m_activeMode.refreshRate = 60;
    m_activeMode.refreshRateHz = 60;
    m_activeMode.scaleFactor = 1.0f;
    m_activeMode.name = "Android Primary Display";
}

AndroidDisplayBackend::~AndroidDisplayBackend() {
    shutdown();
}

bool AndroidDisplayBackend::initialize() {
    if (m_initialized) return true;

    m_activeMode.scaleFactor = sanitizeOutputScale(m_gestalt.display.scale.value_or(1.0f));

    if (initializeAidl()) {
        m_backendKind = BackendKind::AidlComposer3;
        std::cerr << "[AndroidDisplayBackend] selected Composer3 AIDL\n";
        return true;
    }
    shutdownAidl();

    std::cerr << "[AndroidDisplayBackend] Composer3 AIDL unavailable; trying Composer 2.4/2.2 HIDL\n";
    if (m_hidlBackend->initialize(
            m_activeMode.scaleFactor,
            m_gestalt.display.width.value_or(0),
            m_gestalt.display.height.value_or(0),
            m_gestalt.display.refreshRateHz.value_or(0))) {
        m_activeMode = m_hidlBackend->activeMode();
        m_displayId = m_hidlBackend->displayId();
        m_layerId = m_hidlBackend->layerId();
        m_displayConnected = m_hidlBackend->isDisplayConnected();
        m_backendKind = BackendKind::HidlComposer;
        m_initialized = true;
        return true;
    }

    m_backendKind = BackendKind::None;
    return false;
}

bool AndroidDisplayBackend::initializeAidl() {

    m_impl->binderNdkLib = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!m_impl->binderNdkLib) {
        std::cerr << "[AndroidDisplayBackend] Failed to load libbinder_ndk.so\n";
        return false;
    }

    m_impl->getService = reinterpret_cast<pfn_AServiceManager_getService>(
        dlsym(m_impl->binderNdkLib, "AServiceManager_getService"));
    m_impl->startThreadPool = reinterpret_cast<pfn_ABinderProcess_startThreadPool>(
        dlsym(m_impl->binderNdkLib, "ABinderProcess_startThreadPool"));
    m_impl->markVintfStability = reinterpret_cast<pfn_AIBinder_markVintfStability>(
        dlsym(m_impl->binderNdkLib, "AIBinder_markVintfStability"));

    if (m_impl->startThreadPool) {
        m_impl->startThreadPool();
    }

    if (!m_impl->getService) {
        std::cerr << "[AndroidDisplayBackend] AServiceManager_getService symbol missing\n";
        return false;
    }

    // 1. Connect to Composer3 service via standard NDK ServiceManager
    AIBinder* rawBinder = m_impl->getService("android.hardware.graphics.composer3.IComposer/default");
    if (!rawBinder) {
        std::cerr << "[AndroidDisplayBackend] Failed to acquire Composer3 service from ServiceManager\n";
        return false;
    }

    ::ndk::SpAIBinder spComposer(rawBinder);
    m_impl->composer = IComposer::fromBinder(spComposer);
    if (!m_impl->composer) {
        std::cerr << "[AndroidDisplayBackend] IComposer::fromBinder returned null\n";
        return false;
    }

    // 2. Create Composer Client session (retry if SurfaceFlinger client is still being recycled)
    ::ndk::ScopedAStatus createStatus = ::ndk::ScopedAStatus::ok();
    for (int retry = 0; retry < 15; ++retry) {
        createStatus = m_impl->composer->createClient(&m_impl->client);
        if (createStatus.isOk() && m_impl->client) {
            break;
        }
        usleep(200000); // 200ms
    }
    if (!createStatus.isOk() || !m_impl->client) {
        std::cerr << "[AndroidDisplayBackend] createClient failed: " << createStatus.getDescription() << "\n";
        return false;
    }

    // 3. Instantiate callback with VINTF stability and register with HAL
    m_impl->callback = ::ndk::SharedRefBase::make<AndroidComposerCallback>(m_impl.get());
    if (m_impl->markVintfStability) {
        m_impl->markVintfStability(m_impl->callback->asBinder().get());
    }

    auto regStatus = m_impl->client->registerCallback(m_impl->callback);
    if (!regStatus.isOk()) {
        std::cerr << "[AndroidDisplayBackend] registerCallback failed: " << regStatus.getDescription() << "\n";
        return false;
    }

    // 4. Wait for onHotplug event (up to 2 seconds)
    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        m_impl->cv.wait_for(lock, std::chrono::seconds(2), [this] {
            return m_impl->hotplugReceived;
        });
    }

    if (m_impl->hotplugReceived) {
        m_displayId = m_impl->hotplugDisplayId;
        m_displayConnected = m_impl->hotplugConnected;
    } else {
        m_displayId = 0;
        m_displayConnected = true;
    }

    // 5. Query active display configurations dynamically
    std::vector<::aidl::android::hardware::graphics::composer3::DisplayConfiguration> configs;
    auto cfgStatus = m_impl->client->getDisplayConfigurations(m_displayId, 0, &configs);
    if (cfgStatus.isOk() && !configs.empty()) {
        int32_t activeConfigId = -1;
        const auto activeStatus = m_impl->client->getActiveConfig(m_displayId, &activeConfigId);
        auto selected = activeStatus.isOk()
            ? std::find_if(configs.begin(), configs.end(), [activeConfigId](const auto& config) {
                return config.configId == activeConfigId;
            })
            : configs.end();
        if (selected == configs.end()) selected = configs.begin();

        if (m_gestalt.display.hasPreferredResolution()) {
            auto preferred = configs.end();
            int64_t bestRefreshDistance = std::numeric_limits<int64_t>::max();
            for (auto it = configs.begin(); it != configs.end(); ++it) {
                if (it->width != static_cast<int32_t>(*m_gestalt.display.width) ||
                    it->height != static_cast<int32_t>(*m_gestalt.display.height)) {
                    continue;
                }
                const int64_t refreshHz = it->vsyncPeriod > 0
                    ? 1000000000ll / it->vsyncPeriod : 0;
                const int64_t distance = m_gestalt.display.refreshRateHz
                    ? std::llabs(refreshHz - *m_gestalt.display.refreshRateHz) : 0;
                if (preferred == configs.end() || distance < bestRefreshDistance) {
                    preferred = it;
                    bestRefreshDistance = distance;
                }
            }
            if (preferred == configs.end()) {
                std::cerr << "[AndroidDisplayBackend] Gestalt mode "
                          << *m_gestalt.display.width << "x" << *m_gestalt.display.height
                          << " is unavailable; using Composer active mode\n";
            } else if (preferred->configId != selected->configId) {
                const auto setStatus = m_impl->client->setActiveConfig(
                    m_displayId, preferred->configId);
                if (setStatus.isOk()) {
                    selected = preferred;
                } else {
                    std::cerr << "[AndroidDisplayBackend] Composer rejected Gestalt mode: "
                              << setStatus.getDescription() << "\n";
                }
            } else {
                selected = preferred;
            }
        }

        const auto& activeCfg = *selected;
        if (activeCfg.width > 0 && activeCfg.height > 0) {
            m_activeMode.width = activeCfg.width;
            m_activeMode.height = activeCfg.height;
        }
        if (activeCfg.vsyncPeriod > 0) {
            m_activeMode.refreshRateHz = 1000000000 / activeCfg.vsyncPeriod;
            m_activeMode.refreshRate = m_activeMode.refreshRateHz;
        }
        m_activeMode.name = "Android Display " + std::to_string(m_displayId) + " (" +
                            std::to_string(m_activeMode.width) + "x" + std::to_string(m_activeMode.height) + ")";
    }

    // 6. Create Primary Presentation Layer
    int64_t createdLayerId = -1;
    auto layerStatus = m_impl->client->createLayer(m_displayId, 4 /* buffer slot count */, &createdLayerId);
    if (!layerStatus.isOk() || createdLayerId < 0) {
        std::cerr << "[AndroidDisplayBackend] createLayer failed: " << layerStatus.getDescription() << "\n";
        return false;
    }
    m_layerId = createdLayerId;

    const auto vsyncStatus = m_impl->client->setVsyncEnabled(m_displayId, true);
    m_impl->vsyncEnabled = vsyncStatus.isOk();
    if (!vsyncStatus.isOk()) {
        std::cerr << "[AndroidDisplayBackend] Composer VSync callback unavailable: "
                  << vsyncStatus.getDescription() << "\n";
    }

    m_initialized = true;
    return true;
}

void AndroidDisplayBackend::shutdownAidl() {

    for (auto& fences : m_impl->pendingSlotFences) {
        waitAndClearFenceFds(fences);
    }

    if (m_impl->client) {
        (void)m_impl->client->setVsyncEnabled(m_displayId, false);
    }
    if (m_layerId >= 0 && m_impl->client) {
        m_impl->client->destroyLayer(m_displayId, m_layerId);
        m_layerId = -1;
    }

    m_impl->callback.reset();
    m_impl->client.reset();
    m_impl->composer.reset();

    if (m_impl->binderNdkLib) {
        dlclose(m_impl->binderNdkLib);
        m_impl->binderNdkLib = nullptr;
    }

    m_impl->layerBufferSlots.fill(nullptr);
    m_impl->layerBufferHandlesSent.fill(false);
    m_impl->nextLayerBufferSlot = 0;
    m_impl->vsyncSerial = 0;
    m_impl->vsyncEnabled = false;

    m_initialized = false;
}

void AndroidDisplayBackend::shutdown() {
    if (m_backendKind == BackendKind::HidlComposer) {
        m_hidlBackend->shutdown();
    } else {
        shutdownAidl();
    }
    m_backendKind = BackendKind::None;
    m_initialized = false;
    m_displayConnected = false;
    m_layerId = -1;
}

const char* AndroidDisplayBackend::backendName() const {
    switch (m_backendKind) {
    case BackendKind::AidlComposer3: return "aidl-composer3";
    case BackendKind::HidlComposer: return "hidl-composer";
    case BackendKind::None: return "none";
    }
    return "none";
}

bool AndroidDisplayBackend::initHardwareCursor(uint32_t /*width*/, uint32_t /*height*/,
                                               float /*deviceScale*/) {
    return false;
}

bool AndroidDisplayBackend::moveHardwareCursor(int /*x*/, int /*y*/) {
    return false;
}

bool AndroidDisplayBackend::waitForVsync(std::chrono::nanoseconds timeout) {
    if (m_backendKind == BackendKind::HidlComposer) {
        return m_hidlBackend->waitForVsync(timeout);
    }
    return waitForVsyncAidl(timeout);
}

bool AndroidDisplayBackend::waitForVsyncAidl(std::chrono::nanoseconds timeout) {
    if (!m_initialized || !m_impl->client || !m_impl->vsyncEnabled ||
        timeout.count() <= 0) return false;
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    const uint64_t observed = m_impl->vsyncSerial;
    return m_impl->cv.wait_for(lock, timeout, [this, observed] {
        return m_impl->vsyncSerial != observed;
    }) && m_impl->vsyncSerial != observed;
}

bool AndroidDisplayBackend::prepareBufferForRender(AHardwareBuffer* buffer) {
    if (m_backendKind == BackendKind::HidlComposer) {
        return m_hidlBackend->prepareBufferForRender(buffer);
    }
    return prepareBufferForRenderAidl(buffer);
}

bool AndroidDisplayBackend::prepareBufferForRenderAidl(AHardwareBuffer* buffer) {
    if (!m_initialized || !m_impl->client || m_layerId < 0 || !buffer) {
        return false;
    }

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
        waitAndClearFenceFds(m_impl->pendingSlotFences[bufferSlot]);
        m_impl->layerBufferSlots[bufferSlot] = buffer;
        m_impl->layerBufferHandlesSent[bufferSlot] = false;
        m_impl->nextLayerBufferSlot =
            (m_impl->nextLayerBufferSlot + 1) % m_impl->layerBufferSlots.size();
    } else {
        waitAndClearFenceFds(m_impl->pendingSlotFences[bufferSlot]);
    }
    return true;
}

bool AndroidDisplayBackend::presentBuffer(AHardwareBuffer* buffer, int acquireFenceFd) {
    if (m_backendKind == BackendKind::HidlComposer) {
        return m_hidlBackend->presentBuffer(buffer, acquireFenceFd);
    }
    return presentBufferAidl(buffer, acquireFenceFd);
}

bool AndroidDisplayBackend::presentBufferAidl(AHardwareBuffer* buffer, int acquireFenceFd) {
    if (!m_initialized || !m_impl->client || m_layerId < 0 || !buffer) {
        std::cerr << "[AndroidDisplayBackend] presentBuffer invalid state (initialized="
                  << m_initialized << ", layer=" << m_layerId << ", buffer=" << buffer << ")\n";
        return false;
    }

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
        std::cerr << "[AndroidDisplayBackend] scanout buffer was not prepared before present\n";
        return false;
    }

    // 1. Extract and wrap a native handle only when populating a new Composer
    // cache slot. Subsequent frames reference that stable slot directly.
    const native_handle_t* nh = AHardwareBuffer_getNativeHandle(buffer);
    if (!nh) {
        std::cerr << "[AndroidDisplayBackend] AHardwareBuffer_getNativeHandle returned null\n";
        return false;
    }

    // 2. Build Layer Command with full layer state
    LayerCommand layerCmd;
    layerCmd.layer = m_layerId;

    Buffer buf;
    buf.slot = static_cast<int32_t>(bufferSlot);
    if (!m_impl->layerBufferHandlesSent[bufferSlot]) {
        NativeHandle aidlHandle;
        for (int i = 0; i < nh->numFds; ++i) {
            aidlHandle.fds.emplace_back(dup(nh->data[i]));
        }
        for (int i = 0; i < nh->numInts; ++i) {
            aidlHandle.ints.push_back(nh->data[nh->numFds + i]);
        }
        buf.handle = std::move(aidlHandle);
    }
    if (acquireFenceFd >= 0) {
        // The caller retains ownership; the Composer command owns this dup.
        buf.fence = ::ndk::ScopedFileDescriptor(dup(acquireFenceFd));
    } else {
        buf.fence = ::ndk::ScopedFileDescriptor(-1);
    }
    layerCmd.buffer = std::move(buf);

    int32_t w = m_activeMode.width;
    int32_t h = m_activeMode.height;

    Rect df;
    df.left = 0;
    df.top = 0;
    df.right = w;
    df.bottom = h;
    layerCmd.displayFrame = df;

    FRect sc;
    sc.left = 0.0f;
    sc.top = 0.0f;
    sc.right = static_cast<float>(w);
    sc.bottom = static_cast<float>(h);
    layerCmd.sourceCrop = sc;

    layerCmd.composition = ParcelableComposition{Composition::DEVICE};
    layerCmd.blendMode = ParcelableBlendMode{BlendMode::NONE};
    layerCmd.planeAlpha = PlaneAlpha{1.0f};
    layerCmd.z = ZOrder{0};
    layerCmd.dataspace = ParcelableDataspace{Dataspace::UNKNOWN};
    layerCmd.transform = ParcelableTransform{Transform::NONE};

    // 3. Present immediately when the established DEVICE composition remains
    // valid; otherwise this same command performs validation and reports the
    // composition changes required before a second present command.
    DisplayCommand valCmd;
    valCmd.display = m_displayId;
    valCmd.layers.push_back(std::move(layerCmd));
    valCmd.presentOrValidateDisplay = true;

    std::vector<DisplayCommand> valCmds;
    valCmds.push_back(std::move(valCmd));

    std::vector<CommandResultPayload> valResults;
    auto valStatus = m_impl->client->executeCommands(valCmds, &valResults);
    if (!valStatus.isOk()) {
        std::cerr << "[AndroidDisplayBackend] executeCommands(presentOrValidate) failed: "
                  << valStatus.getDescription() << "\n";
        return false;
    }

    bool hasChangedTypes = false;
    bool alreadyPresented = false;
    int commandErrors = 0;
    const auto collectResults = [&](const std::vector<CommandResultPayload>& results,
                                    const char* phase) {
        for (const auto& payload : results) {
            if (payload.getTag() == CommandResultPayload::Tag::error) {
                const auto& err = payload.get<CommandResultPayload::Tag::error>();
                std::cerr << "[AndroidDisplayBackend] " << phase
                          << " CommandError at index " << err.commandIndex
                          << ": code " << err.errorCode << "\n";
                commandErrors++;
            } else if (payload.getTag() ==
                       CommandResultPayload::Tag::changedCompositionTypes) {
                hasChangedTypes = true;
            } else if (payload.getTag() ==
                       CommandResultPayload::Tag::presentOrValidateResult) {
                const auto& result = payload.get<
                    CommandResultPayload::Tag::presentOrValidateResult>();
                alreadyPresented =
                    result.result == PresentOrValidate::Result::Presented;
            } else if (payload.getTag() == CommandResultPayload::Tag::presentFence) {
                const auto& present = payload.get<CommandResultPayload::Tag::presentFence>();
                const int fenceFd = present.fence.get();
                if (fenceFd >= 0) {
                    m_impl->pendingSlotFences[bufferSlot].push_back(dup(fenceFd));
                }
            } else if (payload.getTag() == CommandResultPayload::Tag::releaseFences) {
                const auto& releases = payload.get<CommandResultPayload::Tag::releaseFences>();
                for (const auto& layer : releases.layers) {
                    const int fenceFd = layer.fence.get();
                    if (fenceFd >= 0) {
                        m_impl->pendingSlotFences[bufferSlot].push_back(dup(fenceFd));
                    }
                }
            }
        }
    };
    collectResults(valResults, "PresentOrValidate");

    if (commandErrors > 0) {
        return false;
    }
    m_impl->layerBufferHandlesSent[bufferSlot] = true;
    if (alreadyPresented) return true;

    // 4. Step 2: Present Display (accepting composition changes if requested by HAL)
    DisplayCommand presCmd;
    presCmd.display = m_displayId;
    if (hasChangedTypes) {
        presCmd.acceptDisplayChanges = true;
    }
    presCmd.presentDisplay = true;

    std::vector<DisplayCommand> presCmds;
    presCmds.push_back(std::move(presCmd));

    std::vector<CommandResultPayload> presResults;
    auto presStatus = m_impl->client->executeCommands(presCmds, &presResults);
    if (!presStatus.isOk()) {
        std::cerr << "[AndroidDisplayBackend] executeCommands(present) failed: " << presStatus.getDescription() << "\n";
        return false;
    }

    collectResults(presResults, "Present");

    return (commandErrors == 0);
}

} // namespace lcl::platform::android
