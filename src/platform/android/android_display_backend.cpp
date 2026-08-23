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
};

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

    m_activeMode.scaleFactor = resolveOutputScale();

    if (initializeAidl()) {
        m_backendKind = BackendKind::AidlComposer3;
        std::cerr << "[AndroidDisplayBackend] selected Composer3 AIDL\n";
        return true;
    }
    shutdownAidl();

    std::cerr << "[AndroidDisplayBackend] Composer3 AIDL unavailable; trying Composer 2.4/2.2 HIDL\n";
    if (m_hidlBackend->initialize(m_activeMode.scaleFactor)) {
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
        const auto& activeCfg = configs.front();
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

    m_initialized = true;
    return true;
}

void AndroidDisplayBackend::shutdownAidl() {

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

    // 1. Extract and wrap native handle
    const native_handle_t* nh = AHardwareBuffer_getNativeHandle(buffer);
    if (!nh) {
        std::cerr << "[AndroidDisplayBackend] AHardwareBuffer_getNativeHandle returned null\n";
        return false;
    }

    NativeHandle aidlHandle;
    for (int i = 0; i < nh->numFds; ++i) {
        aidlHandle.fds.emplace_back(dup(nh->data[i]));
    }
    for (int i = 0; i < nh->numInts; ++i) {
        aidlHandle.ints.push_back(nh->data[nh->numFds + i]);
    }

    // 2. Build Layer Command with full layer state
    LayerCommand layerCmd;
    layerCmd.layer = m_layerId;

    Buffer buf;
    buf.slot = 0;
    buf.handle = std::move(aidlHandle);
    if (acquireFenceFd >= 0) {
        buf.fence = ::ndk::ScopedFileDescriptor(acquireFenceFd);
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

    // 3. Step 1: Validate Display
    DisplayCommand valCmd;
    valCmd.display = m_displayId;
    valCmd.layers.push_back(std::move(layerCmd));
    valCmd.validateDisplay = true;

    std::vector<DisplayCommand> valCmds;
    valCmds.push_back(std::move(valCmd));

    std::vector<CommandResultPayload> valResults;
    auto valStatus = m_impl->client->executeCommands(valCmds, &valResults);
    if (!valStatus.isOk()) {
        std::cerr << "[AndroidDisplayBackend] executeCommands(validate) failed: " << valStatus.getDescription() << "\n";
        return false;
    }

    bool hasChangedTypes = false;
    int commandErrors = 0;
    for (const auto& payload : valResults) {
        if (payload.getTag() == CommandResultPayload::Tag::error) {
            const auto& err = payload.get<CommandResultPayload::Tag::error>();
            std::cerr << "[AndroidDisplayBackend] Validate CommandError at index "
                      << err.commandIndex << ": code " << err.errorCode << "\n";
            commandErrors++;
        } else if (payload.getTag() == CommandResultPayload::Tag::changedCompositionTypes) {
            hasChangedTypes = true;
        }
    }

    if (commandErrors > 0) {
        return false;
    }

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

    for (auto& payload : presResults) {
        if (payload.getTag() == CommandResultPayload::Tag::error) {
            const auto& err = payload.get<CommandResultPayload::Tag::error>();
            std::cerr << "[AndroidDisplayBackend] Present CommandError at index "
                      << err.commandIndex << ": code " << err.errorCode << "\n";
            commandErrors++;
        } else if (payload.getTag() == CommandResultPayload::Tag::presentFence) {
            auto& pf = payload.get<CommandResultPayload::Tag::presentFence>();
            int fenceFd = pf.fence.get();
            if (fenceFd >= 0) {
                struct pollfd pfd;
                pfd.fd = fenceFd;
                pfd.events = POLLIN | POLLPRI;
                poll(&pfd, 1, 1000);
            }
        }
    }

    return (commandErrors == 0);
}

} // namespace lcl::platform::android
