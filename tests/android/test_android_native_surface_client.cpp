#include "lcl-client/surface_client.hpp"
#include "system/render/client_egl_context.hpp"

#include <android/hardware_buffer.h>
#include <GLES3/gl3.h>

#include <chrono>
#include <iostream>
#include <optional>
#include <thread>
#include <unistd.h>

int main() {
    constexpr uint32_t kWidth = 360;
    constexpr uint32_t kHeight = 240;
    lcl::client::SurfaceOptions options{};
    options.surfaceId = 1;
    options.appId = "org.lcl.test.native-ahb-client";
    options.title = "Native AHB SurfaceClient";
    options.bounds = {120.0f, 180.0f, static_cast<float>(kWidth),
                      static_cast<float>(kHeight)};

    lcl::client::SurfaceClient surface;
    if (!surface.connect(options)) {
        std::cerr << "android_native_client_connect=no\n";
        return 1;
    }

    lcl::render::ClientEGLContext context;
    bool initialized = false;
    bool submitted = false;
    bool presented = false;
    bool released = false;
    bool closeRequested = false;
    uint32_t bufferId = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline &&
           !(presented && released)) {
        for (auto& event : surface.dispatch()) {
            if (auto* release =
                    std::get_if<lcl::client::BufferReleasedEvent>(&event)) {
                if (release->bufferId == bufferId) {
                    context.releaseDmaBuf(
                        static_cast<uint32_t>(bufferId),
                        release->releaseFence.release());
                    released = true;
                }
            } else if (std::get_if<lcl::client::FramePresentedEvent>(&event)) {
                presented = true;
            }
        }
        if (presented && !closeRequested) {
            closeRequested = surface.requestSurfaceClose();
        }
        if (!submitted && surface.hasConfigure() && surface.rasterFd() >= 0) {
            const auto& configured = surface.configure();
            const uint32_t width = static_cast<uint32_t>(
                configured.backingWidth * configured.bufferScale);
            const uint32_t height = static_cast<uint32_t>(
                configured.backingHeight * configured.bufferScale);
            if (!initialized) {
                initialized = context.initialize(width, height);
            }
            const auto target = initialized
                ? context.acquireDmaBufTarget() : std::nullopt;
            if (!target) return 3;
            glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
            glViewport(0, 0, static_cast<GLsizei>(target->width),
                       static_cast<GLsizei>(target->height));
            glClearColor(0.1f, 0.35f, 0.9f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const auto exported = context.exportCurrentDmaBuf();
            if (!exported || !exported->androidHardwareBuffer) return 4;
            bufferId = exported->bufferId;
            lcl::client::PlatformNativeFrame frame{};
            frame.bufferId = exported->bufferId;
            frame.contentRevision = 1;
            frame.width = exported->width;
            frame.height = exported->height;
            frame.stride = exported->stride;
            frame.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            frame.damage = {0.0f, 0.0f, configured.bounds.width,
                            configured.bounds.height};
            frame.opaque = true;
            frame.acquireFence =
                lcl::client::OwnedFd(exported->acquireFenceFd);
            frame.writeHandle = [&context, bufferId](int sidebandFd) {
                return context.sendNativeBufferHandle(
                    sidebandFd, static_cast<uint32_t>(bufferId));
            };
            submitted = surface.submitFrame(std::move(frame));
            if (!submitted) {
                context.releaseDmaBuf(static_cast<uint32_t>(bufferId));
                return 5;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!closeRequested) (void)surface.requestSurfaceClose();
    std::cout << "android_native_client_submit="
              << (submitted ? "yes" : "no") << "\n"
              << "android_native_client_presented="
              << (presented ? "yes" : "no") << "\n"
              << "android_native_client_released="
              << (released ? "yes" : "no") << "\n";
    return submitted && presented && released ? 0 : 6;
}
