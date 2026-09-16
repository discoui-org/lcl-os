#include "lcl-client/surface_client.hpp"
#include "lcl-gpu/gpu_client.hpp"
#include "system/ipc/gpu_protocol.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

int main() {
    constexpr const char* kAppId = "org.lcl.gpu-presentation-probe";
    const char* instanceText = std::getenv("LCL_APP_INSTANCE_ID");
    const char* appId = std::getenv("LCL_APP_ID");
    char* end = nullptr;
    const uint64_t instanceId = instanceText ? std::strtoull(instanceText, &end, 10) : 0;
    if (!appId || std::string_view(appId) != kAppId || !instanceText ||
        end == instanceText || *end != '\0' || instanceId == 0) {
        std::cerr << "gpu_presentation_rejected_direct_launch\n";
        return 7;
    }
    lcl::client::SurfaceClient surface;
    lcl::client::SurfaceOptions options{};
    options.surfaceId = 0x47505501;
    options.appId = kAppId;
    options.appInstanceId = instanceId;
    options.title = "GPU Presentation PoC";
    options.bounds = {80.0f, 120.0f, 640.0f, 400.0f};
    if (!surface.connect(options)) return 1;

    lcl::gpu::GpuClient gpu;
    if (!gpu.connect() || !gpu.beginHandshake(0x47505501)) return 2;
    lcl::gpu::DeviceCapabilities capabilities{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool submitted = false;
    bool presented = false;
    bool released = false;
    bool closeRequested = false;
    std::chrono::steady_clock::time_point presentedAt{};
    std::chrono::steady_clock::time_point releasedAt{};
    uint64_t bufferId = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (capabilities.maxImageDimension2D == 0) {
            const auto handshake = gpu.dispatch(capabilities);
            if (handshake == lcl::gpu::HandshakeStatus::ProtocolError ||
                handshake == lcl::gpu::HandshakeStatus::Closed ||
                handshake == lcl::gpu::HandshakeStatus::IoError) return 3;
        }
        for (auto& event : surface.dispatch()) {
            if (const auto* frame = std::get_if<lcl::client::FramePresentedEvent>(&event)) {
                presented = frame->message.frameSerial != 0;
                if (presented && presentedAt.time_since_epoch().count() == 0) {
                    presentedAt = std::chrono::steady_clock::now();
                }
            } else if (auto* release = std::get_if<lcl::client::BufferReleasedEvent>(&event)) {
                if (release->bufferId == bufferId) {
                    released = gpu.releasePresentedBuffer(bufferId, std::move(release->releaseFence));
                    if (released) releasedAt = std::chrono::steady_clock::now();
                }
            }
        }
        if (!submitted && surface.hasConfigure() && surface.hasFrameCredit() &&
            capabilities.maxImageDimension2D != 0) {
            const auto& configure = surface.configure();
            // Configure bounds are logical units. A native AHardwareBuffer is
            // physical storage, so derive its extent from the compositor's
            // scale rather than the compatibility backing fields (which are
            // logical for regular top-level surfaces).
            const uint32_t width = std::max(1u, static_cast<uint32_t>(
                std::ceil(configure.bounds.width * configure.bufferScale)));
            const uint32_t height = std::max(1u, static_cast<uint32_t>(
                std::ceil(configure.bounds.height * configure.bufferScale)));
            lcl::gpu::ClearedNativeBuffer buffer{};
            if (!gpu.clearColor(1, width, height,
                                lcl::gpu_protocol::kAndroidHardwareBufferRgba8888,
                                lcl::gpu_protocol::kAndroidHardwareBufferGpuSampledColorOutput,
                                0.08f, 0.42f, 0.95f, 1.0f, buffer)) return 4;
            bufferId = buffer.bufferId;
            lcl::client::PlatformNativeFrame frame{};
            frame.bufferId = buffer.bufferId;
            frame.contentRevision = buffer.contentRevision;
            frame.width = buffer.width;
            frame.height = buffer.height;
            frame.stride = buffer.stride;
            frame.format = buffer.format;
            frame.damage = {0.0f, 0.0f, configure.bounds.width, configure.bounds.height};
            frame.opaque = true;
            frame.acquireFence = std::move(buffer.acquireFence);
            frame.writeHandle = [&gpu, id = buffer.bufferId](int sidebandFd) {
                return gpu.deliverNativeBuffer(id, sidebandFd);
            };
            submitted = surface.submitFrame(std::move(frame));
            if (!submitted) return 5;
        }
        // A presented native buffer remains owned by the compositor until its
        // surface stops sampling it. Keep the clear visible briefly, then use
        // the ordinary close protocol so rasterd can return its release fence
        // to gpud; waiting for a release before closing would deadlock a
        // single-buffer producer.
        if (presented && !closeRequested &&
            std::chrono::steady_clock::now() - presentedAt >= std::chrono::seconds(3)) {
            closeRequested = surface.requestSurfaceClose();
            if (!closeRequested) return 6;
        }
        // Keep the submitted surface alive long enough for an operator to see
        // the clear. The release still proves gpud got the compositor fence;
        // keeping the surface does not retain the producer's AHB reference.
        if (presented && released &&
            std::chrono::steady_clock::now() - releasedAt >= std::chrono::seconds(5)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cout << "gpu_presentation_submitted=" << submitted << '\n'
              << "gpu_presentation_presented=" << presented << '\n'
              << "gpu_presentation_released=" << released << '\n';
    return submitted && presented && released ? 0 : 6;
}
