#include <iostream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <dlfcn.h>

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

    class BnComposerCallback : public IComposerCallback {
    public:
        BnComposerCallback();
        virtual ~BnComposerCallback();
        virtual ndk::ScopedAStatus onHotplug(int64_t display, bool connected) = 0;
        virtual ndk::ScopedAStatus onRefresh(int64_t display) = 0;
        virtual ndk::ScopedAStatus onVsync(int64_t display, int64_t timestamp, int32_t vsyncPeriodNanos) = 0;
        virtual ndk::ScopedAStatus onSeamlessPossible(int64_t display) = 0;
    };
}

extern "C" {
    typedef AIBinder* (*pfn_AServiceManager_getService)(const char* instance);
    typedef void (*pfn_ABinderProcess_startThreadPool)();

    std::shared_ptr<void> _ZN4aidl7android8hardware8graphics9composer39IComposer10fromBinderERKN3ndk10SpAIBinderE(const ndk::SpAIBinder& binder);
    void _ZN4aidl7android8hardware8graphics9composer310BpComposer12createClientEPNSt3__110shared_ptrINS3_15IComposerClientEEE(ndk::ScopedAStatus* outStatus, void* self, std::shared_ptr<void>* outClient);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient16registerCallbackERKNSt3__110shared_ptrINS3_17IComposerCallbackEEE(ndk::ScopedAStatus* outStatus, void* self, const std::shared_ptr<aidl::android::hardware::graphics::composer3::IComposerCallback>& callback);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient11createLayerEliPl(ndk::ScopedAStatus* outStatus, void* self, int64_t display, int32_t bufferSlotCount, int64_t* outLayer);
}

static std::mutex g_mutex;
static std::condition_variable g_cv;
static bool g_hotplug = false;
static int64_t g_dispId = -1;

class MyCallback : public aidl::android::hardware::graphics::composer3::BnComposerCallback {
public:
    ndk::ScopedAStatus onHotplug(int64_t display, bool connected) override {
        std::cout << ">>> REAL ONHOTPLUG CALLBACK: display=" << display << ", connected=" << connected << " <<<\n";
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dispId = display;
        g_hotplug = true;
        g_cv.notify_all();
        return ndk::ScopedAStatus(AStatus_fromStatus(STATUS_OK));
    }
    ndk::ScopedAStatus onRefresh(int64_t display) override {
        std::cout << ">>> ONREFRESH: display=" << display << "\n";
        return ndk::ScopedAStatus(AStatus_fromStatus(STATUS_OK));
    }
    ndk::ScopedAStatus onVsync(int64_t display, int64_t timestamp, int32_t vsyncPeriodNanos) override {
        return ndk::ScopedAStatus(AStatus_fromStatus(STATUS_OK));
    }
    ndk::ScopedAStatus onSeamlessPossible(int64_t display) override {
        return ndk::ScopedAStatus(AStatus_fromStatus(STATUS_OK));
    }
};

int main() {
    void* binderNdk = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!binderNdk) return 1;

    auto getService = reinterpret_cast<pfn_AServiceManager_getService>(
        dlsym(binderNdk, "AServiceManager_getService"));
    auto startThreadPool = reinterpret_cast<pfn_ABinderProcess_startThreadPool>(
        dlsym(binderNdk, "ABinderProcess_startThreadPool"));
    if (startThreadPool) startThreadPool();

    AIBinder* raw = getService("android.hardware.graphics.composer3.IComposer/default");
    ndk::SpAIBinder sp(raw);
    auto composer = _ZN4aidl7android8hardware8graphics9composer39IComposer10fromBinderERKN3ndk10SpAIBinderE(sp);

    std::shared_ptr<void> client;
    ndk::ScopedAStatus status;
    _ZN4aidl7android8hardware8graphics9composer310BpComposer12createClientEPNSt3__110shared_ptrINS3_15IComposerClientEEE(&status, composer.get(), &client);

    std::cout << "createClient: isOk=" << (status.isOk() ? "YES" : "NO") << "\n";
    if (!status.isOk()) return 1;

    auto cb = std::make_shared<MyCallback>();
    ndk::ScopedAStatus regStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient16registerCallbackERKNSt3__110shared_ptrINS3_17IComposerCallbackEEE(&regStatus, client.get(), cb);

    std::cout << "registerCallback: isOk=" << (regStatus.isOk() ? "YES" : "NO") << "\n";

    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv.wait_for(lock, std::chrono::seconds(2), [] { return g_hotplug; });
    }

    std::cout << "Hotplug received: " << (g_hotplug ? "YES" : "NO") << ", displayId=" << g_dispId << "\n";

    if (g_hotplug) {
        int64_t layerId = 0;
        ndk::ScopedAStatus layerStatus;
        _ZN4aidl7android8hardware8graphics9composer316BpComposerClient11createLayerEliPl(&layerStatus, client.get(), g_dispId, 2, &layerId);
        std::cout << "createLayer on display " << g_dispId << ": isOk=" << (layerStatus.isOk() ? "YES" : "NO")
                  << ", layerId=" << layerId
                  << ", Ex=" << layerStatus.getExceptionCode()
                  << ", SSE=" << layerStatus.getServiceSpecificError() << "\n";
    }

    return 0;
}
