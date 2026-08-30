#include <iostream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <cstring>
#include <vector>

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
    AStatus** getR() { return &mStatus; }
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
    class DisplayCommand;
    class CommandResultPayload;
}

extern "C" {
    typedef AIBinder* (*pfn_AServiceManager_getService)(const char* instance);
    typedef void (*pfn_ABinderProcess_startThreadPool)();
    typedef void (*pfn_AIBinder_markVintfStability)(AIBinder* binder);

    void _ZN4aidl7android8hardware8graphics9composer39IComposer10fromBinderERKN3ndk10SpAIBinderE(std::shared_ptr<void>* outComposer, const ndk::SpAIBinder& binder);
    void _ZN4aidl7android8hardware8graphics9composer310BpComposer12createClientEPNSt3__110shared_ptrINS3_15IComposerClientEEE(ndk::ScopedAStatus* outStatus, void* self, std::shared_ptr<void>* outClient);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient16registerCallbackERKNSt3__110shared_ptrINS3_17IComposerCallbackEEE(ndk::ScopedAStatus* outStatus, void* self, const std::shared_ptr<aidl::android::hardware::graphics::composer3::IComposerCallback>& callback);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient11createLayerEliPl(ndk::ScopedAStatus* outStatus, void* self, int64_t display, int32_t bufferSlotCount, int64_t* outLayer);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient17getDisplayConfigsElPNSt3__16vectorIiNS5_9allocatorIiEEEE(ndk::ScopedAStatus* outStatus, void* self, int64_t display, std::vector<int32_t>* outConfigs);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient12destroyLayerEll(ndk::ScopedAStatus* outStatus, void* self, int64_t display, int64_t layer);
    void _ZN4aidl7android8hardware8graphics9composer316BpComposerClient15executeCommandsERKNSt3__16vectorINS3_14DisplayCommandENS5_9allocatorIS7_EEEEPNS6_INS3_20CommandResultPayloadENS8_ISD_EEEE(
        ndk::ScopedAStatus* outStatus, void* self, const void* commandsVec, void* resultsVec);
}

static std::mutex g_mutex;
static std::condition_variable g_cv;
static bool g_hotplug = false;
static int64_t g_dispId = -1;
static AIBinder* g_rawCb = nullptr;

extern "C" {
    void* cb_onCreate(void* args) { return args ? args : (void*)0x1; }
    void cb_onDestroy(void* /*userData*/) {}

    void my_asBinder(void** out, void* self) {
        if (g_rawCb) AIBinder_incStrong(g_rawCb);
        *out = g_rawCb;
    }

    bool my_isRemote(void* self) {
        return false;
    }

    void my_dummy(ndk::ScopedAStatus* outStatus, void* self) {
        AStatus* s = AStatus_fromStatus(STATUS_OK);
        *outStatus = ndk::ScopedAStatus(s);
    }
}

static binder_status_t onCallbackTransact(AIBinder* /*binder*/, transaction_code_t code, const AParcel* in, AParcel* /*out*/) {
    if (code == 1) { // onHotplug(long display, bool connected)
        int64_t display = 0;
        bool connected = false;
        AParcel_readInt64(in, &display);
        AParcel_readBool(in, &connected);
        std::cout << "\n======================================================\n"
                  << ">>> [TRANSACT EVENT] onHotplug: display=" << display
                  << ", connected=" << (connected ? "TRUE" : "FALSE") << " <<<\n"
                  << "======================================================\n";
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_dispId = display;
            g_hotplug = true;
            g_cv.notify_all();
        }
    }
    return STATUS_OK;
}

int main() {
    void* binderNdk = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!binderNdk) return 1;

    auto getService = reinterpret_cast<pfn_AServiceManager_getService>(
        dlsym(binderNdk, "AServiceManager_getService"));
    auto startThreadPool = reinterpret_cast<pfn_ABinderProcess_startThreadPool>(
        dlsym(binderNdk, "ABinderProcess_startThreadPool"));
    auto markVintfStability = reinterpret_cast<pfn_AIBinder_markVintfStability>(
        dlsym(binderNdk, "AIBinder_markVintfStability"));

    if (startThreadPool) startThreadPool();

    AIBinder* raw = getService("android.hardware.graphics.composer3.IComposer/default");
    ndk::SpAIBinder sp(raw);
    std::shared_ptr<void> composer;
    _ZN4aidl7android8hardware8graphics9composer39IComposer10fromBinderERKN3ndk10SpAIBinderE(&composer, sp);

    std::cout << "composer = " << composer.get() << "\n";
    if (!composer) return 1;

    std::shared_ptr<void> client;
    ndk::ScopedAStatus status;
    _ZN4aidl7android8hardware8graphics9composer310BpComposer12createClientEPNSt3__110shared_ptrINS3_15IComposerClientEEE(&status, composer.get(), &client);

    std::cout << "createClient: isOk=" << (status.isOk() ? "YES" : "NO") << "\n";
    if (!status.isOk()) return 1;

    static AIBinder_Class* cbClass = AIBinder_Class_define(
        "android.hardware.graphics.composer3.IComposerCallback", cb_onCreate, cb_onDestroy, onCallbackTransact);
    g_rawCb = AIBinder_new(cbClass, (void*)0x1);
    
    if (markVintfStability && g_rawCb) {
        markVintfStability(g_rawCb);
    }

    static void* myVtable[32];
    for (int i = 0; i < 30; ++i) {
        myVtable[i] = (void*)&my_dummy;
    }
    myVtable[2] = (void*)&my_asBinder;
    myVtable[3] = (void*)&my_isRemote;

    struct CallbackWrapper {
        void* vptr;
    };
    auto cbObj = std::make_shared<CallbackWrapper>();
    cbObj->vptr = myVtable;
    auto typedCb = reinterpret_cast<std::shared_ptr<aidl::android::hardware::graphics::composer3::IComposerCallback>&>(cbObj);

    ndk::ScopedAStatus regStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient16registerCallbackERKNSt3__110shared_ptrINS3_17IComposerCallbackEEE(&regStatus, client.get(), typedCb);

    std::cout << "registerCallback: isOk=" << (regStatus.isOk() ? "YES" : "NO") << "\n";

    // Wait for onHotplug event from Composer3
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv.wait_for(lock, std::chrono::seconds(2), [] { return g_hotplug; });
    }

    std::cout << "HOTPLUG RESULT: received=" << (g_hotplug ? "YES" : "NO")
              << ", displayId=" << g_dispId << "\n";

    int64_t layerId = 0;
    ndk::ScopedAStatus layerStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient11createLayerEliPl(&layerStatus, client.get(), g_dispId, 2, &layerId);
    std::cout << "CREATE_LAYER on " << g_dispId << ": isOk=" << (layerStatus.isOk() ? "YES" : "NO")
              << ", layerId=" << layerId << "\n";

    // Create 1 DisplayCommand object
    // Size of DisplayCommand is 0x138 (312 bytes)
    alignas(16) char dispCmd[512];
    std::memset(dispCmd, 0, sizeof(dispCmd));
    *reinterpret_cast<int64_t*>(dispCmd) = g_dispId; // display = 0

    // Set validateDisplay = true (offset 0x128)
    dispCmd[0x128] = 1;
    // Set presentDisplay = true (offset 0x129)
    dispCmd[0x129] = 1;

    // Vector with 1 DisplayCommand element
    struct VectorBuffer {
        char* begin;
        char* end;
        char* cap;
    };
    VectorBuffer cmdVec{dispCmd, dispCmd + 312, dispCmd + 312};
    VectorBuffer resVec{nullptr, nullptr, nullptr};

    ndk::ScopedAStatus execStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient15executeCommandsERKNSt3__16vectorINS3_14DisplayCommandENS5_9allocatorIS7_EEEEPNS6_INS3_20CommandResultPayloadENS8_ISD_EEEE(
        &execStatus, client.get(), &cmdVec, &resVec);

    std::cout << "executeCommands: isOk=" << (execStatus.isOk() ? "YES" : "NO")
              << ", Ex=" << execStatus.getExceptionCode()
              << ", SSE=" << execStatus.getServiceSpecificError()
              << ", Msg=" << (execStatus.getMessage() ? execStatus.getMessage() : "none") << "\n";

    ndk::ScopedAStatus destroyStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient12destroyLayerEll(&destroyStatus, client.get(), g_dispId, layerId);
    std::cout << "destroyLayer: isOk=" << (destroyStatus.isOk() ? "YES" : "NO") << "\n";

    return 0;
}
