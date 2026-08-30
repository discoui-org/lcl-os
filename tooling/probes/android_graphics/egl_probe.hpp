#pragma once

namespace lcl::probe {

struct EglGlesInfo {
    const char* eglVendor{nullptr};
    const char* eglVersion{nullptr};
    const char* eglClientApis{nullptr};
    const char* eglExtensions{nullptr};

    const char* glVendor{nullptr};
    const char* glRenderer{nullptr};
    const char* glVersion{nullptr};
    const char* glShadingLanguageVersion{nullptr};
    const char* glExtensions{nullptr};

    bool isSupported{false};
};

EglGlesInfo runEglGlesProbe();

} // namespace lcl::probe
