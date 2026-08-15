#include "platform/android/android_display_backend.hpp"

#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <cstring>

namespace ndk {
class SpAIBinder {
public:
    SpAIBinder() : mBinder(nullptr) {}
    explicit SpAIBinder(AIBinder* binder) : mBinder(binder) {}
    SpAIBinder(const SpAIBinder& other) {
        mBinder = other.mBinder;
        if (mBinder) AIBinder_incStrong(mBinder);
    }
    ~SpAIBinder() {
        if (mBinder) AIBinder_decStrong(mBinder);
    }
    AIBinder* get() const { return mBinder; }
    void set(AIBinder* b) {
        if (mBinder) AIBinder_decStrong(mBinder);
        mBinder = b;
    }
private:
    AIBinder* mBinder{nullptr};
};

class ScopedAStatus {
public:
    ScopedAStatus() : mStatus(nullptr) {}
    explicit ScopedAStatus(AStatus* status) : mStatus(status) {}
    ~ScopedAStatus() { if (mStatus) AStatus_delete(mStatus); }
    AStatus* get() const { return mStatus; }
    bool isOk() const { return AStatus_isOk(mStatus); }
    int32_t getExceptionCode() const { return AStatus_getExceptionCode(mStatus); }
    int32_t getServiceSpecificError() const { return AStatus_getServiceSpecificError(mStatus); }
    const char* getMessage() const { return AStatus_getMessage(mStatus); }
private:
    AStatus* mStatus{nullptr};
};
}

namespace aidl::android::hardware::graphics::composer3 {
    class IComposerCallback {
    public:
        virtual ~IComposerCallback() = default;
    };
}

extern "C" {
    typedef AIBinder* (*pfn_AServiceManager_getService)(const char* instance);
    typedef void (*pfn_ABinderProcess_startThreadPool)();
    typedef void (*pfn_AIBinder_markVintfStability)(AIBinder* binder);

    void _ZN4aidl7android8hardware8graphics9composer39IComposer10fromBinderERKN3ndk10SpAIBinderE(std::shared_ptr<void>* outComposer, const ndk::SpAIBinder& binder);
    void _ZN4aidl7android8hardware8graphics9composer310BpComposer12createClientEPNSt3__110shared_ptrINS3_15IComposerClientEEE(ndk::ScopedAStatus* outStatus, void* self, std::shared_ptr<void>* outClient);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient16registerCallbackERKNSt3__110shared_ptrINS3_17IComposerCallbackEEE(ndk::ScopedAStatus* outStatus, void* self, const std::shared_ptr<aidl::android::hardware::graphics::composer3::IComposerCallback>& callback);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient17getDisplayConfigsElPNSt3__16vectorIiNS5_9allocatorIiEEEE(ndk::ScopedAStatus* outStatus, void* self, int64_t display, std::vector<int32_t>* outConfigs);
}

namespace lcl::platform::android {

struct AndroidDisplayBackend::Impl {
    void* binderNdkLib{nullptr};
    pfn_AServiceManager_getService getService{nullptr};
    pfn_ABinderProcess_startThreadPool startThreadPool{nullptr};
    pfn_AIBinder_markVintfStability markVintfStability{nullptr};

    std::shared_ptr<void> composer;
    std::shared_ptr<void> client;
    AIBinder* rawCallbackBinder{nullptr};

    std::mutex mutex;
    std::condition_variable cv;
    bool hotplugReceived{false};
    int64_t hotplugDisplayId{-1};
    bool hotplugConnected{false};

    void* callbackVtable[32]{nullptr};
};

static AndroidDisplayBackend::Impl* g_currentDisplayBackendImpl = nullptr;

extern "C" {
    static void* backend_cb_onCreate(void* args) { return args ? args : (void*)0x1; }
    static void backend_cb_onDestroy(void* /*userData*/) {}

    static void backend_cb_asBinder(void** out, void* /*self*/) {
        if (g_currentDisplayBackendImpl && g_currentDisplayBackendImpl->rawCallbackBinder) {
            AIBinder_incStrong(g_currentDisplayBackendImpl->rawCallbackBinder);
            *out = g_currentDisplayBackendImpl->rawCallbackBinder;
        } else {
            *out = nullptr;
        }
    }

    static bool backend_cb_isRemote(void* /*self*/) {
        return false;
    }

    static void backend_cb_dummy(ndk::ScopedAStatus* outStatus, void* /*self*/) {
        AStatus* s = AStatus_fromStatus(STATUS_OK);
        *outStatus = ndk::ScopedAStatus(s);
    }
}

static binder_status_t backendOnCallbackTransact(AIBinder* /*binder*/, transaction_code_t code, const AParcel* in, AParcel* /*out*/) {
    if (code == 1 && g_currentDisplayBackendImpl) { // onHotplug(long display, bool connected)
        int64_t displayId = 0;
        bool connected = false;
        if (AParcel_readInt64(in, &displayId) == STATUS_OK) {
            AParcel_readBool(in, &connected);
            std::lock_guard<std::mutex> lock(g_currentDisplayBackendImpl->mutex);
            g_currentDisplayBackendImpl->hotplugDisplayId = displayId;
            g_currentDisplayBackendImpl->hotplugConnected = connected;
            g_currentDisplayBackendImpl->hotplugReceived = true;
            g_currentDisplayBackendImpl->cv.notify_all();
        }
    }
    return STATUS_OK;
}

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

    g_currentDisplayBackendImpl = m_impl.get();

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

    // 1. Connect to Composer3 service
    AIBinder* rawComposer = m_impl->getService("android.hardware.graphics.composer3.IComposer/default");
    if (!rawComposer) {
        std::cerr << "[AndroidDisplayBackend] Failed to acquire Composer3 service\n";
        return false;
    }

    ndk::SpAIBinder spComposer(rawComposer);
    _ZN4aidl7android8hardware8graphics9composer39IComposer10fromBinderERKN3ndk10SpAIBinderE(&m_impl->composer, spComposer);
    if (!m_impl->composer) {
        std::cerr << "[AndroidDisplayBackend] IComposer::fromBinder returned null\n";
        return false;
    }

    // 2. Create Composer Client
    ndk::ScopedAStatus createStatus;
    _ZN4aidl7android8hardware8graphics9composer310BpComposer12createClientEPNSt3__110shared_ptrINS3_15IComposerClientEEE(&createStatus, m_impl->composer.get(), &m_impl->client);
    if (!createStatus.isOk() || !m_impl->client) {
        std::cerr << "[AndroidDisplayBackend] createClient failed (Ex=" << createStatus.getExceptionCode()
                  << ", SSE=" << createStatus.getServiceSpecificError() << ")\n";
        return false;
    }

    // 3. Register Callback with VINTF stability
    static AIBinder_Class* cbClass = AIBinder_Class_define(
        "android.hardware.graphics.composer3.IComposerCallback", backend_cb_onCreate, backend_cb_onDestroy, backendOnCallbackTransact);
    m_impl->rawCallbackBinder = AIBinder_new(cbClass, (void*)0x1);
    if (m_impl->markVintfStability && m_impl->rawCallbackBinder) {
        m_impl->markVintfStability(m_impl->rawCallbackBinder);
    }

    for (int i = 0; i < 30; ++i) {
        m_impl->callbackVtable[i] = (void*)&backend_cb_dummy;
    }
    m_impl->callbackVtable[2] = (void*)&backend_cb_asBinder;
    m_impl->callbackVtable[3] = (void*)&backend_cb_isRemote;

    struct CallbackWrapper {
        void* vptr;
    };
    auto cbObj = std::make_shared<CallbackWrapper>();
    cbObj->vptr = m_impl->callbackVtable;
    auto typedCb = reinterpret_cast<std::shared_ptr<aidl::android::hardware::graphics::composer3::IComposerCallback>&>(cbObj);

    ndk::ScopedAStatus regStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient16registerCallbackERKNSt3__110shared_ptrINS3_17IComposerCallbackEEE(&regStatus, m_impl->client.get(), typedCb);
    if (!regStatus.isOk()) {
        std::cerr << "[AndroidDisplayBackend] registerCallback failed\n";
        return false;
    }

    // 4. Wait for onHotplug event
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

    // 5. Query display configurations
    std::vector<int32_t> configs;
    ndk::ScopedAStatus cfgStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient17getDisplayConfigsElPNSt3__16vectorIiNS5_9allocatorIiEEEE(&cfgStatus, m_impl->client.get(), m_displayId, &configs);
    if (cfgStatus.isOk() && !configs.empty()) {
        m_activeMode.name = "Android Display " + std::to_string(m_displayId);
    }

    m_initialized = true;
    return true;
}

void AndroidDisplayBackend::shutdown() {
    if (!m_initialized) return;

    if (g_currentDisplayBackendImpl == m_impl.get()) {
        g_currentDisplayBackendImpl = nullptr;
    }

    if (m_impl->rawCallbackBinder) {
        AIBinder_decStrong(m_impl->rawCallbackBinder);
        m_impl->rawCallbackBinder = nullptr;
    }

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
