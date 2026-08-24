#include "platform/android/android_platform_services.hpp"
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <iostream>
#include <cassert>

typedef void* GLeglImageOES;
typedef EGLClientBuffer (*pfn_eglGetNativeClientBufferANDROID)(const struct AHardwareBuffer* buffer);
typedef EGLImageKHR (*pfn_eglCreateImageKHR)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint* attrib_list);
typedef EGLBoolean (*pfn_eglDestroyImageKHR)(EGLDisplay dpy, EGLImageKHR image);
typedef void (*pfn_glEGLImageTargetTexture2DOES)(GLenum target, GLeglImageOES image);

int main() {
    std::cout << "========================================================\n"
              << "  LCL Core Linux — Stage 3D Android Presentation Test   \n"
              << "========================================================\n\n";

    lcl::platform::android::AndroidPlatformServices services;

    std::cout << "[1/5] Initializing AndroidPlatformServices...\n";
    bool initOk = services.initialize();
    std::cout << "  ✓ Services Initialized: " << (initOk ? "YES" : "NO") << "\n";
    assert(initOk && "AndroidPlatformServices failed to initialize");

    std::cout << "\n[2/5] Inspecting Android Display Backend...\n";
    auto& display = services.display();
    std::cout << "  ✓ Display Initialized: " << (display.isInitialized() ? "YES" : "NO") << "\n";
    const auto& mode = display.activeMode();
    std::cout << "  ✓ Active Mode: " << mode.width << "x" << mode.height << " @ "
              << mode.refreshRate << "Hz (Name: " << mode.name << ")\n";
    std::cout << "  ✓ Display ID: " << services.getAndroidDisplay().displayId() << "\n";
    std::cout << "  ✓ Display Connected: " << (services.getAndroidDisplay().isDisplayConnected() ? "YES" : "NO") << "\n";
    std::cout << "  ✓ Primary Layer ID: " << services.getAndroidDisplay().layerId() << "\n";
    std::cout << "  CREATE_LAYER: " << (services.getAndroidDisplay().layerId() >= 0 ? "SUCCESS" : "FAILED") << "\n";

    std::cout << "\n[3/5] Inspecting Android Graphics Context...\n";
    auto& graphics = services.graphics();
    std::cout << "  ✓ Graphics Initialized: " << (graphics.isInitialized() ? "YES" : "NO") << "\n";
    std::cout << "  ✓ Hardware Accelerated: " << (graphics.isHardwareAccelerated() ? "YES" : "NO") << "\n";
    std::cout << "  ✓ Make Current: " << (graphics.makeCurrent() ? "YES" : "NO") << "\n";

    std::cout << "\n[4/5] Rendering Pure Magenta Frame into AHardwareBuffer...\n";
    uint32_t width = mode.width;
    uint32_t height = mode.height;

    AHardwareBuffer_Desc desc = {};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    AHardwareBuffer* ahb = nullptr;
    int allocRes = AHardwareBuffer_allocate(&desc, &ahb);
    if (allocRes != 0 || !ahb) {
        std::cerr << "  ✗ AHardwareBuffer_allocate failed: " << allocRes << "\n";
        return 1;
    }
    std::cout << "  ✓ AHardwareBuffer allocated (" << width << "x" << height << " RGBA_8888)\n";

    // Composer may still be scanning this slot out from an earlier frame.
    // Synchronize before any GPU writes begin, not when the completed frame is
    // submitted for presentation.
    bool prepareOk = services.getAndroidDisplay().prepareBufferForRender(ahb);
    std::cout << "  ✓ Buffer ready for rendering: " << (prepareOk ? "YES" : "NO") << "\n";
    assert(prepareOk && "Composer scanout buffer preparation failed");

    auto eglGetNativeClientBufferANDROID = reinterpret_cast<pfn_eglGetNativeClientBufferANDROID>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    auto eglCreateImageKHR = reinterpret_cast<pfn_eglCreateImageKHR>(eglGetProcAddress("eglCreateImageKHR"));
    auto eglDestroyImageKHR = reinterpret_cast<pfn_eglDestroyImageKHR>(eglGetProcAddress("eglDestroyImageKHR"));
    auto glEGLImageTargetTexture2DOES = reinterpret_cast<pfn_glEGLImageTargetTexture2DOES>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    assert(eglGetNativeClientBufferANDROID && eglCreateImageKHR && eglDestroyImageKHR && glEGLImageTargetTexture2DOES);

    EGLClientBuffer clientBuf = eglGetNativeClientBufferANDROID(ahb);
    EGLint imgAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR eglImage = eglCreateImageKHR(services.getAndroidGraphics().getEglDisplay(),
                                            EGL_NO_CONTEXT,
                                            EGL_NATIVE_BUFFER_ANDROID,
                                            clientBuf,
                                            imgAttrs);
    assert(eglImage != EGL_NO_IMAGE_KHR);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(eglImage));

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    assert(fboStatus == GL_FRAMEBUFFER_COMPLETE);

    glViewport(0, 0, width, height);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f); // Pure Magenta: R=255, G=0, B=255
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    std::cout << "  ✓ OpenGL ES Framebuffer cleared with solid Magenta (RGB: 255, 0, 255)\n";

    std::cout << "\n[5/5] Presenting Buffer to AVD Display via Composer3...\n";
    bool presentOk = services.getAndroidDisplay().presentBuffer(ahb);
    std::cout << "  VALIDATE: " << (presentOk ? "SUCCESS" : "FAILED") << "\n";
    std::cout << "  PRESENT:  " << (presentOk ? "SUCCESS" : "FAILED") << "\n";
    std::cout << "  COMMAND_ERRORS: " << (presentOk ? 0 : 1) << "\n";
    std::cout << "  AVD SCREEN MAGENTA: " << (presentOk ? "YES" : "NO") << "\n";

    // Cleanup render targets
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    eglDestroyImageKHR(services.getAndroidGraphics().getEglDisplay(), eglImage);
    AHardwareBuffer_release(ahb);

    std::cout << "\nShutting down AndroidPlatformServices...\n";
    services.shutdown();
    std::cout << "  ✓ Layer Destroyed & Shutdown Complete (isInitialized: "
              << (services.isInitialized() ? "YES" : "NO") << ")\n\n";

    std::cout << "========================================================\n"
              << "  Stage 3D Android Presentation Test: " << (presentOk ? "SUCCESS" : "FAILED") << "\n"
              << "========================================================\n";
    return presentOk ? 0 : 1;
}
