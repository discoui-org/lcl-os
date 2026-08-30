#include "direct_presentation_probe.hpp"

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>
#include <unistd.h>

namespace lcl::probe {

// Simple Binder definitions matching libbinder ABI
struct RefBase {
    virtual ~RefBase() = default;
    void* mRefs{nullptr};
};

struct IBinder : public RefBase {
    virtual int32_t pingBinder() = 0;
    virtual int32_t dump(int fd, const void* args) = 0;
    // transact
};

// We dynamically resolve libbinder entry points
typedef void* (*pfn_defaultServiceManager)();
typedef void* (*pfn_ProcessState_self)();
typedef void (*pfn_ProcessState_startThreadPool)(void* self);

DirectPresentationResult runDirectPresentationProbe(bool isSurfaceFlingerRunning) {
    DirectPresentationResult result{};
    std::ostringstream details;

    void* libbinder = dlopen("libbinder.so", RTLD_NOW);
    if (!libbinder) {
        result.details = "dlopen(libbinder.so) failed: " + std::string(dlerror());
        return result;
    }

    // Check if SurfaceFlinger is running or stopped via service check
    // We can also test through popen / system commands to inspect the exact parcel response
    FILE* fp = popen("service check android.hardware.graphics.composer3.IComposer/default", "r");
    char buf[512];
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            if (strstr(buf, "found")) {
                result.composerServiceReachable = true;
            }
        }
        pclose(fp);
    }

    if (result.composerServiceReachable) {
        // Test calling createClient (code 1)
        FILE* callFp = popen("service call android.hardware.graphics.composer3.IComposer/default 1", "r");
        if (callFp) {
            std::string callOutput;
            while (fgets(buf, sizeof(buf), callFp)) {
                callOutput += buf;
            }
            pclose(callFp);

            // Check if call output contains exception code or binder object
            if (callOutput.find("Result: Parcel(") != std::string::npos) {
                if (callOutput.find("fffffff8") != std::string::npos && callOutput.find("00000006") != std::string::npos) {
                    result.clientCreated = false;
                    result.clientCreateStatus = 6;
                    result.clientCreateErrorMsg = "EX_SERVICE_SPECIFIC: 6 (NO_RESOURCES - SurfaceFlinger holds exclusive ownership)";
                    details << "createClient failed: EX_SERVICE_SPECIFIC 6 (NO_RESOURCES) because SurfaceFlinger is holding the exclusive Composer session.";
                } else if (callOutput.find("Object #0") != std::string::npos || callOutput.find("00000000") != std::string::npos) {
                    result.clientCreated = true;
                    result.clientCreateStatus = 0;
                    details << "createClient SUCCESS! IComposerClient binder session established (SurfaceFlinger is not holding the session).";
                } else {
                    details << "createClient returned: " << callOutput;
                }
            }
        }
    }

    dlclose(libbinder);
    result.details = details.str();
    return result;
}

} // namespace lcl::probe
