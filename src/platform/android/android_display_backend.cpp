#include "platform/android/android_display_backend.hpp"

#include <aidl/android/hardware/graphics/composer3/IComposer.h>
#include <aidl/android/hardware/graphics/composer3/IComposerClient.h>
#include <aidl/android/hardware/graphics/composer3/BnComposerCallback.h>
#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>

#include <iostream>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>

namespace lcl::platform::android {

using aidl::android::hardware::graphics::composer3::IComposer;
using aidl::android::hardware::graphics::composer3::IComposerClient;
using aidl::android::hardware::graphics::composer3::BnComposerCallback;
using aidl::android::hardware::graphics::composer3::VsyncPeriodChangeTimeline;
using aidl::android::hardware::graphics::composer3::RefreshRateChangedDebugData;
using aidl::android::hardware::graphics::common::DisplayHotplugEvent;
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
    : m_impl(std::make_unique<Impl>()) {
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
        std::cerr << "[AndroidDisplayBackend] Failed to acquire Composer3 service\n";
        return false;
    }

    ::ndk::SpAIBinder spComposer(rawBinder);
    m_impl->composer = IComposer::fromBinder(spComposer);
    if (!m_impl->composer) {
        std::cerr << "[AndroidDisplayBackend] IComposer::fromBinder returned null\n";
        return false;
    }

    // 2. Create Composer Client session
    auto createStatus = m_impl->composer->createClient(&m_impl->client);
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

    // 5. Query active display configurations
    std::vector<int32_t> configs;
    auto cfgStatus = m_impl->client->getDisplayConfigs(m_displayId, &configs);
    if (cfgStatus.isOk() && !configs.empty()) {
        m_activeMode.name = "Android Display " + std::to_string(m_displayId);
    }

    m_initialized = true;
    return true;
}

void AndroidDisplayBackend::shutdown() {
    if (!m_initialized) return;

    m_impl->callback.reset();
    m_impl->client.reset();
    m_impl->composer.reset();

    if (m_impl->binderNdkLib) {
        dlclose(m_impl->binderNdkLib);
        m_impl->binderNdkLib = nullptr;
    }

    m_initialized = false;
}

bool AndroidDisplayBackend::initHardwareCursor(uint32_t /*width*/, uint32_t /*height*/) {
    return false;
}

bool AndroidDisplayBackend::moveHardwareCursor(int /*x*/, int /*y*/) {
    return false;
}

} // namespace lcl::platform::android
