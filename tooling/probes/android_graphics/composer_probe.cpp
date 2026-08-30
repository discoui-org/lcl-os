#include "composer_probe.hpp"

#include <android/binder_ibinder.h>
#include <dlfcn.h>
#include <iostream>

namespace lcl::probe {

typedef AIBinder* (*pfn_AServiceManager_checkService)(const char* instance);
typedef AIBinder* (*pfn_AServiceManager_getService)(const char* instance);
typedef bool (*pfn_AIBinder_isAlive)(const AIBinder* binder);
typedef void (*pfn_AIBinder_decStrong)(AIBinder* binder);

ComposerProbeResult runComposerProbe() {
    ComposerProbeResult result{};
    result.serviceName = "android.hardware.graphics.composer3.IComposer/default";

    void* handle = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!handle) {
        result.details = "Failed to dlopen libbinder_ndk.so: " + std::string(dlerror());
        return result;
    }
    result.binderNdkLoaded = true;

    auto checkService = reinterpret_cast<pfn_AServiceManager_checkService>(
        dlsym(handle, "AServiceManager_checkService"));
    auto getService = reinterpret_cast<pfn_AServiceManager_getService>(
        dlsym(handle, "AServiceManager_getService"));
    auto isAlive = reinterpret_cast<pfn_AIBinder_isAlive>(
        dlsym(handle, "AIBinder_isAlive"));
    auto decStrong = reinterpret_cast<pfn_AIBinder_decStrong>(
        dlsym(handle, "AIBinder_decStrong"));

    if (!checkService && !getService) {
        result.details = "Failed to resolve AServiceManager symbols from libbinder_ndk.so";
        dlclose(handle);
        return result;
    }

    AIBinder* binder = nullptr;
    if (checkService) {
        binder = checkService(result.serviceName.c_str());
    } else if (getService) {
        binder = getService(result.serviceName.c_str());
    }

    if (binder != nullptr) {
        result.serviceReachable = true;
        bool alive = isAlive ? isAlive(binder) : true;
        result.details = "Successfully acquired AIBinder for " + result.serviceName +
                         " (Alive: " + (alive ? "YES" : "NO") + ")";
        if (decStrong) {
            decStrong(binder);
        }
    } else {
        result.serviceReachable = false;
        result.details = "Service not found or permission denied for " + result.serviceName;
    }

    dlclose(handle);
    return result;
}

} // namespace lcl::probe
