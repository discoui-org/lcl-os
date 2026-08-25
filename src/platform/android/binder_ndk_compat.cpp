#include <android/binder_stability.h>

#include <dlfcn.h>

#include <cstdio>
#include <mutex>

extern "C" void lcl_AIBinder_markVintfStability(AIBinder* binder) {
    using MarkVintfStability = void (*)(AIBinder*);
    static MarkVintfStability mark = [] {
        void* library = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return static_cast<MarkVintfStability>(nullptr);
        return reinterpret_cast<MarkVintfStability>(
            dlsym(library, "AIBinder_markVintfStability"));
    }();
    if (mark) {
        mark(binder);
        return;
    }

    static std::once_flag reportOnce;
    std::call_once(reportOnce, [] {
        std::fputs("[Android Binder] Runtime VINTF stability symbol unavailable.\n",
                   stderr);
    });
}
