#include "system/render/client_egl_context.hpp"

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cstdint>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using GetNativeClientBuffer = EGLClientBuffer (*)(const AHardwareBuffer*);
using CreateImage = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum,
                                    EGLClientBuffer, const EGLint*);
using DestroyImage = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using ImageTargetTexture = void (*)(GLenum, void*);
}

int main() {
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 48;
    lcl::render::ClientEGLContext context;
    if (!context.initialize(kWidth, kHeight) || !context.hasDmaBufPool()) {
        std::cerr << "android_client_ahb_pool=no\n";
        return 1;
    }

    const auto target = context.acquireDmaBufTarget();
    if (!target) {
        std::cerr << "android_client_ahb_acquire=no\n";
        return 2;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
    glViewport(0, 0, kWidth, kHeight);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const auto frame = context.exportCurrentDmaBuf();
    if (!frame || !frame->androidHardwareBuffer) {
        std::cerr << "android_client_ahb_export=no\n";
        return 3;
    }

    int sockets[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0 ||
        !context.sendNativeBufferHandle(sockets[0], frame->bufferId)) {
        std::cerr << "android_client_ahb_send=no\n";
        return 4;
    }
    AHardwareBuffer* received = nullptr;
    if (AHardwareBuffer_recvHandleFromUnixSocket(sockets[1], &received) != 0 ||
        !received) {
        std::cerr << "android_client_ahb_receive=no\n";
        return 5;
    }

    const auto getNativeClientBuffer = reinterpret_cast<GetNativeClientBuffer>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    const auto createImage = reinterpret_cast<CreateImage>(
        eglGetProcAddress("eglCreateImageKHR"));
    const auto destroyImage = reinterpret_cast<DestroyImage>(
        eglGetProcAddress("eglDestroyImageKHR"));
    const auto imageTarget = reinterpret_cast<ImageTargetTexture>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!getNativeClientBuffer || !createImage || !destroyImage || !imageTarget) {
        std::cerr << "android_client_ahb_import_api=no\n";
        return 6;
    }

    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = createImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                    EGL_NATIVE_BUFFER_ANDROID,
                                    getNativeClientBuffer(received), attributes);
    GLuint texture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    imageTarget(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    glFinish();
    uint8_t pixel[4]{};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const bool sharedPixel = pixel[0] > 240 && pixel[1] < 16 &&
                             pixel[2] > 240 && pixel[3] > 240;

    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR) destroyImage(eglGetCurrentDisplay(), image);
    AHardwareBuffer_release(received);
    close(sockets[0]);
    close(sockets[1]);
    if (frame->acquireFenceFd >= 0) close(frame->acquireFenceFd);
    context.releaseDmaBuf(frame->bufferId);

    std::cout << "android_client_ahb_pool=yes\n"
              << "android_client_ahb_handle_transfer=yes\n"
              << "android_client_ahb_shared_pixel="
              << (sharedPixel ? "yes" : "no") << "\n";
    return sharedPixel ? 0 : 7;
}
