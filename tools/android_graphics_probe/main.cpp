#include <iostream>
#include <iomanip>

#include "egl_probe.hpp"
#include "ahb_probe.hpp"
#include "ipc_probe.hpp"
#include "composer_probe.hpp"
#include "onscreen_test_pattern.hpp"

int main() {
    std::cout << "========================================================\n"
              << "  LCL Core Linux — Android Graphics & HAL Probe (Native)\n"
              << "  Target: Android 16 / API 36 / x86_64\n"
              << "========================================================\n\n";

    // 1. EGL + GLES Native Probe
    std::cout << "[1/5] Running EGL & OpenGL ES Probe...\n";
    auto egl = lcl::probe::runEglGlesProbe();
    if (egl.isSupported) {
        std::cout << "  ✓ EGL Vendor:   " << (egl.eglVendor ? egl.eglVendor : "N/A") << "\n"
                  << "  ✓ EGL Version:  " << (egl.eglVersion ? egl.eglVersion : "N/A") << "\n"
                  << "  ✓ EGL APIs:     " << (egl.eglClientApis ? egl.eglClientApis : "N/A") << "\n"
                  << "  ✓ GL Vendor:    " << (egl.glVendor ? egl.glVendor : "N/A") << "\n"
                  << "  ✓ GL Renderer:  " << (egl.glRenderer ? egl.glRenderer : "N/A") << "\n"
                  << "  ✓ GL Version:   " << (egl.glVersion ? egl.glVersion : "N/A") << "\n"
                  << "  ✓ GLSL Version: " << (egl.glShadingLanguageVersion ? egl.glShadingLanguageVersion : "N/A") << "\n";
    } else {
        std::cerr << "  ✗ EGL/GLES Probe Failed!\n";
    }
    std::cout << "\n";

    // 2. AHardwareBuffer Allocation & EGLImage GPU Render
    std::cout << "[2/5] Running AHardwareBuffer & EGLImage GPU Render Probe...\n";
    auto ahb = lcl::probe::runAhbRenderProbe();
    if (ahb.allocationSuccess) {
        std::cout << "  ✓ AHB Allocated: " << ahb.width << "x" << ahb.height
                  << " (Stride: " << ahb.stride << ", Format: " << ahb.format << ")\n";
    } else {
        std::cerr << "  ✗ AHB Allocation Failed!\n";
    }
    if (ahb.eglImageImportSuccess) {
        std::cout << "  ✓ AHB -> EGLImageKHR Import: SUCCESS\n";
    } else {
        std::cerr << "  ✗ AHB -> EGLImageKHR Import Failed!\n";
    }
    if (ahb.glRenderSuccess) {
        std::cout << "  ✓ GLES FBO Render: SUCCESS\n";
        std::cout << "  ✓ glReadPixels Match: " << (ahb.pixelMatchGlReadPixels ? "YES (Pixel: 0x" : "NO (Pixel: 0x")
                  << std::hex << ahb.renderedPixelHex << std::dec << ")\n";
        std::cout << "  ✓ CPU Buffer Lock Match: " << (ahb.pixelMatchCpuLock ? "YES" : "NO") << "\n";
    } else {
        std::cerr << "  ✗ GLES FBO Render Failed!\n";
    }
    std::cout << "\n";

    // 3. AHardwareBuffer Unix Socket IPC Probe
    std::cout << "[3/5] Running AHardwareBuffer Process-to-Process Unix Socket IPC Probe...\n";
    auto ipc = lcl::probe::runAhbIpcProbe();
    if (ipc.socketTransferSuccess) {
        std::cout << "  ✓ AHardwareBuffer_sendHandleToUnixSocket: SUCCESS\n";
    } else {
        std::cerr << "  ✗ AHardwareBuffer_sendHandleToUnixSocket Failed!\n";
    }
    if (ipc.handleReceivedValid) {
        std::cout << "  ✓ AHardwareBuffer_recvHandleFromUnixSocket: SUCCESS\n";
        std::cout << "  ✓ Child Process Zero-Copy Pixel Match: YES (0x"
                  << std::hex << ipc.receivedPixelHex << std::dec << ")\n";
    } else {
        std::cerr << "  ✗ AHardwareBuffer_recvHandleFromUnixSocket Failed!\n";
    }
    std::cout << "\n";

    // 4. Composer3 / Binder NDK Probe
    std::cout << "[4/5] Running Composer3 / Binder NDK Probe...\n";
    auto composer = lcl::probe::runComposerProbe();
    std::cout << "  ✓ libbinder_ndk.so: " << (composer.binderNdkLoaded ? "LOADED" : "FAILED") << "\n"
              << "  ✓ Service Reachable: " << (composer.serviceReachable ? "YES" : "NO") << "\n"
              << "  ✓ Details: " << composer.details << "\n\n";

    // 5. Magenta Frame Presentation Probe
    std::cout << "[5/5] Running Direct Composer3 Magenta Presentation Probe...\n";
    auto magenta = lcl::probe::runMagentaFramePresentation();
    std::cout << "  ✓ IComposerClient Created: " << (magenta.composerClientCreated ? "YES" : "NO") << "\n"
              << "  ✓ Callback Registered:     " << (magenta.callbackRegistered ? "YES" : "NO") << "\n"
              << "  ✓ onHotplug Received:      " << (magenta.hotplugReceived ? "YES" : "NO") << "\n"
              << "  ✓ Display ID:              " << magenta.displayId << "\n"
              << "  ✓ Layer Created:           " << (magenta.layerCreated ? "YES" : "NO") << " (Layer ID: " << magenta.layerId << ")\n"
              << "  ✓ AHB Magenta Rendered:    " << (magenta.ahbRendered ? "YES" : "NO") << "\n"
              << "  ✓ Commands Executed:       " << (magenta.commandsExecuted ? "YES" : "NO") << "\n"
              << "  ✓ Presentation Success:    " << (magenta.presentSuccess ? "YES" : "NO") << "\n";
    if (!magenta.details.empty()) {
        std::cout << "  ✓ Details: " << magenta.details << "\n";
    }
    std::cout << "\n";

    std::cout << "========================================================\n"
              << "  Probe Execution Completed.\n"
              << "========================================================\n";

    return 0;
}
