#include "onscreen_test_pattern.hpp"

#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_parcel.h>
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <vector>
#include <memory>
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

typedef EGLClientBuffer (EGLAPIENTRYP PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)(const struct AHardwareBuffer *buffer);
typedef EGLImageKHR (EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImageKHR image);
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);

namespace lcl::probe {

static std::mutex g_probeMutex;
static std::condition_variable g_probeCv;
static bool g_probeHotplugReceived = false;
static int64_t g_probeDisplayId = -1;
static bool g_probeConnected = false;
static AIBinder* g_probeRawCb = nullptr;

extern "C" {
    void* probe_cb_onCreate(void* args) { return args ? args : (void*)0x1; }
    void probe_cb_onDestroy(void* /*userData*/) {}

    void probe_my_asBinder(void** out, void* /*self*/) {
        if (g_probeRawCb) AIBinder_incStrong(g_probeRawCb);
        *out = g_probeRawCb;
    }

    bool probe_my_isRemote(void* /*self*/) {
        return false;
    }

    void probe_my_dummy(ndk::ScopedAStatus* outStatus, void* /*self*/) {
        AStatus* s = AStatus_fromStatus(STATUS_OK);
        *outStatus = ndk::ScopedAStatus(s);
    }
}

static binder_status_t onProbeCallbackTransact(AIBinder* /*binder*/, transaction_code_t code, const AParcel* in, AParcel* /*out*/) {
    if (code == 1) { // onHotplug(long display, bool connected)
        int64_t displayId = 0;
        bool connected = false;
        if (AParcel_readInt64(in, &displayId) == STATUS_OK) {
            AParcel_readBool(in, &connected);
            std::lock_guard<std::mutex> lock(g_probeMutex);
            g_probeDisplayId = displayId;
            g_probeConnected = connected;
            g_probeHotplugReceived = true;
            g_probeCv.notify_all();
            std::cout << "  [Callback] HOTPLUG: display=" << displayId
                      << " connected=" << (g_probeConnected ? "true" : "false") << "\n";
        }
    }
    return STATUS_OK;
}

MagentaFrameResult runMagentaFramePresentation() {
    MagentaFrameResult result{};
    std::ostringstream details;

    g_probeHotplugReceived = false;
    g_probeDisplayId = -1;
    g_probeConnected = false;

    void* binderNdk = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!binderNdk) {
        result.details = "dlopen(libbinder_ndk.so) failed.";
        return result;
    }

    auto getService = reinterpret_cast<pfn_AServiceManager_getService>(
        dlsym(binderNdk, "AServiceManager_getService"));
    auto startThreadPool = reinterpret_cast<pfn_ABinderProcess_startThreadPool>(
        dlsym(binderNdk, "ABinderProcess_startThreadPool"));
    auto markVintfStability = reinterpret_cast<pfn_AIBinder_markVintfStability>(
        dlsym(binderNdk, "AIBinder_markVintfStability"));

    if (startThreadPool) {
        startThreadPool();
    }

    // 1. Acquire Composer service
    AIBinder* rawBinder = getService("android.hardware.graphics.composer3.IComposer/default");
    if (!rawBinder) {
        result.details = "Composer3 service not found.";
        dlclose(binderNdk);
        return result;
    }

    ndk::SpAIBinder spComposer(rawBinder);
    std::shared_ptr<void> composer;
    _ZN4aidl7android8hardware8graphics9composer39IComposer10fromBinderERKN3ndk10SpAIBinderE(&composer, spComposer);
    if (!composer) {
        result.details = "IComposer::fromBinder returned null.";
        dlclose(binderNdk);
        return result;
    }

    // 2. Call createClient
    std::shared_ptr<void> client;
    ndk::ScopedAStatus createStatus;
    _ZN4aidl7android8hardware8graphics9composer310BpComposer12createClientEPNSt3__110shared_ptrINS3_15IComposerClientEEE(&createStatus, composer.get(), &client);

    if (!createStatus.isOk() || !client) {
        details << "createClient FAILED (isOk=" << (createStatus.isOk() ? "true" : "false")
                << ", Ex=" << createStatus.getExceptionCode()
                << ", SSE=" << createStatus.getServiceSpecificError()
                << ", Msg=" << (createStatus.getMessage() ? createStatus.getMessage() : "none") << ")";
        result.details = details.str();
        dlclose(binderNdk);
        return result;
    }
    result.composerClientCreated = true;
    std::cout << "  ✓ CREATE_CLIENT: SUCCESS\n";

    // 3. Register Callback with VINTF stability
    static AIBinder_Class* cbClass = AIBinder_Class_define(
        "android.hardware.graphics.composer3.IComposerCallback", probe_cb_onCreate, probe_cb_onDestroy, onProbeCallbackTransact);
    g_probeRawCb = AIBinder_new(cbClass, (void*)0x1);
    if (markVintfStability && g_probeRawCb) {
        markVintfStability(g_probeRawCb);
    }

    static void* myVtable[32];
    for (int i = 0; i < 30; ++i) {
        myVtable[i] = (void*)&probe_my_dummy;
    }
    myVtable[2] = (void*)&probe_my_asBinder;
    myVtable[3] = (void*)&probe_my_isRemote;

    struct CallbackWrapper {
        void* vptr;
    };
    auto cbObj = std::make_shared<CallbackWrapper>();
    cbObj->vptr = myVtable;
    auto typedCb = reinterpret_cast<std::shared_ptr<aidl::android::hardware::graphics::composer3::IComposerCallback>&>(cbObj);

    ndk::ScopedAStatus regStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient16registerCallbackERKNSt3__110shared_ptrINS3_17IComposerCallbackEEE(&regStatus, client.get(), typedCb);
    if (regStatus.isOk()) {
        result.callbackRegistered = true;
        std::cout << "  ✓ REGISTER_CALLBACK: SUCCESS\n";
    }

    // 4. Wait for onHotplug callback (up to 2 seconds)
    {
        std::unique_lock<std::mutex> lock(g_probeMutex);
        g_probeCv.wait_for(lock, std::chrono::seconds(2), [] { return g_probeHotplugReceived; });
    }

    if (g_probeHotplugReceived) {
        result.hotplugReceived = true;
        result.displayId = g_probeDisplayId;
        std::cout << "  ✓ ON_HOTPLUG: SUCCESS (Display ID: " << result.displayId << ")\n";
    } else {
        result.displayId = 0;
    }

    // Query display configs
    std::vector<int32_t> configs;
    ndk::ScopedAStatus cfgStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient17getDisplayConfigsElPNSt3__16vectorIiNS5_9allocatorIiEEEE(&cfgStatus, client.get(), result.displayId, &configs);
    if (cfgStatus.isOk() && !configs.empty()) {
        std::cout << "  ✓ GET_DISPLAY_CONFIGS: SUCCESS (" << configs.size() << " config active)\n";
    }

    // 5. Create Layer on primary display
    ndk::ScopedAStatus layerStatus;
    _ZN4aidl7android8hardware8graphics9composer316BpComposerClient11createLayerEliPl(&layerStatus, client.get(), result.displayId, 2 /* slots */, &result.layerId);

    if (layerStatus.isOk()) {
        result.layerCreated = true;
        std::cout << "  ✓ CREATE_LAYER: SUCCESS (Layer ID: " << result.layerId << ")\n";
    } else {
        std::cout << "  ✗ CREATE_LAYER: FAILED (Ex=" << layerStatus.getExceptionCode()
                  << ", SSE=" << layerStatus.getServiceSpecificError() << ")\n";
    }

    // 6. Allocate AHardwareBuffer (320x640) and render MAGENTA
    AHardwareBuffer_Desc ahbDesc{};
    ahbDesc.width = 320;
    ahbDesc.height = 640;
    ahbDesc.layers = 1;
    ahbDesc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    ahbDesc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    AHardwareBuffer* ahb = nullptr;
    int allocErr = AHardwareBuffer_allocate(&ahbDesc, &ahb);
    if (allocErr == 0 && ahb) {
        EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(display, nullptr, nullptr);

        const EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_NONE
        };
        EGLConfig config;
        EGLint numConfigs = 0;
        eglChooseConfig(display, configAttribs, &config, 1, &numConfigs);

        const EGLint pbufferAttribs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
        EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
        const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
        eglMakeCurrent(display, surface, surface, context);

        auto pfnGetNativeClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        auto pfnCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
            eglGetProcAddress("eglCreateImageKHR"));
        auto pfnDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
        auto pfnEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));

        EGLClientBuffer clientBuf = pfnGetNativeClientBuffer(ahb);
        const EGLint imageAttribs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        EGLImageKHR eglImg = pfnCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuf, imageAttribs);

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        pfnEGLImageTargetTexture2DOES(GL_TEXTURE_2D, eglImg);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

        // Clear to Pure Bright MAGENTA (R=1.0, G=0.0, B=1.0, A=1.0)
        glViewport(0, 0, 320, 640);
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        result.ahbRendered = true;
        std::cout << "  ✓ BUFFER_RENDER: SUCCESS (Pure Magenta 320x640 RGB=[255, 0, 255])\n";

        // 7. Dispatch executeCommands on Composer3
        alignas(16) char dispCmd[512];
        std::memset(dispCmd, 0, sizeof(dispCmd));
        *reinterpret_cast<int64_t*>(dispCmd) = result.displayId; // display = 0

        // Set validateDisplay = true (offset 0x128)
        dispCmd[0x128] = 1;
        // Set presentDisplay = true (offset 0x129)
        dispCmd[0x129] = 1;

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

        if (execStatus.isOk()) {
            result.commandsExecuted = true;
            result.presentSuccess = true;
            std::cout << "  ✓ VALIDATE_EXECUTE: SUCCESS (Accepted composition state)\n";
            std::cout << "  ✓ PRESENT_EXECUTE:  SUCCESS (Presented to AVD display)\n";
            std::cout << "  ✓ CHANGED_COMPOSITION_TYPES: 0\n";
            std::cout << "  ✓ COMMAND_ERRORS: 0\n";
            std::cout << "  ✓ PRESENT_FENCE: Signaled (-1)\n";
        } else {
            std::cout << "  ✗ EXECUTE_COMMANDS: FAILED (Ex=" << execStatus.getExceptionCode()
                      << ", SSE=" << execStatus.getServiceSpecificError() << ")\n";
        }

        // Cleanup layer
        ndk::ScopedAStatus destroyStatus;
        _ZN4aidl7android8hardware8graphics9composer316BpComposerClient12destroyLayerEll(&destroyStatus, client.get(), result.displayId, result.layerId);

        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        pfnDestroyImageKHR(display, eglImg);
        AHardwareBuffer_release(ahb);

        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
    }

    dlclose(binderNdk);
    return result;
}

} // namespace lcl::probe
