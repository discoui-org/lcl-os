#include "lcl-gfxstream/android_host_ingress.hpp"

#include "GfxStreamAgents.h"
#include "RendererImpl.h"
#include "vulkan/vulkan_gfxstream_structure_type.h"
#include "vulkan/VkAndroidNativeBuffer.h"
#include "FrameBuffer.h"
#include "FrameworkFormats.h"
#include "gfxstream/host/Features.h"
#include "host-common/misc.h"
#include "render-utils/RenderChannel.h"

#include <fcntl.h>
#include <android/hardware_buffer.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>
#include <unordered_map>
#include <vector>

namespace lcl::gfxstream {
namespace {

constexpr std::size_t kMaximumSocketPacketBytes = 60 * 1024;
constexpr int kPumpTimeoutMilliseconds = 1;

bool setNonBlocking(const int descriptor) noexcept {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

void closeReceivedDescriptors(msghdr& message) noexcept {
    for (cmsghdr* control = CMSG_FIRSTHDR(&message); control != nullptr;
         control = CMSG_NXTHDR(&message, control)) {
        if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t byteCount = control->cmsg_len - CMSG_LEN(0);
        const std::size_t descriptorCount = byteCount / sizeof(int);
        auto* descriptors = reinterpret_cast<int*>(CMSG_DATA(control));
        for (std::size_t index = 0; index < descriptorCount; ++index) {
            if (descriptors[index] >= 0) close(descriptors[index]);
        }
    }
}

bool receivePacket(const int descriptor, std::vector<std::uint8_t>& destination) noexcept {
    std::array<std::uint8_t, kMaximumSocketPacketBytes> bytes{};
    std::array<std::uint8_t, CMSG_SPACE(sizeof(int) * 4)> controls{};
    iovec data{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = controls.data();
    message.msg_controllen = controls.size();
    const ssize_t count = recvmsg(descriptor, &message, MSG_DONTWAIT);
    if (count <= 0) return false;
    const bool invalid = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
                         message.msg_controllen != 0;
    if (message.msg_controllen != 0) closeReceivedDescriptors(message);
    if (invalid) return false;
    destination.assign(bytes.begin(), bytes.begin() + count);
    return true;
}

bool sendPacket(const int descriptor, const std::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.empty() || bytes.size() > kMaximumSocketPacketBytes) return false;
    const ssize_t count = send(descriptor, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    return count == static_cast<ssize_t>(bytes.size());
}

void enable(::gfxstream::host::FeatureInfo& feature) {
    feature.enabled = true;
    feature.reason = "LCL Android headless Vulkan ingress";
}

::gfxstream::host::FeatureSet makeFeatures() {
    ::gfxstream::host::FeatureSet features;
    // Mirrors the Vulkan-only subset selected by upstream's renderer flags.
    // No external blob, system blob, GL, or native swapchain capability is
    // enabled until a later LCL presentation bridge can authorize it.
    enable(features.GuestVulkanOnly);
    enable(features.Vulkan);
    // The guest socket mode has no RenderControl feature exchange.  Keep
    // every optional wire-format feature disabled until that negotiation is
    // available; otherwise host and guest disagree on command layouts.
    return features;
}

void initializeHeadlessAgents() {
    // RendererImpl normally receives these from the Android Emulator outer
    // process.  FrameBuffer still creates an internal logical display even
    // when there is no native window, so register upstream's in-memory agents
    // before initializing it.  They own no Android UI or LCL compositor
    // state; this is only gfxstream's required headless bookkeeping.
    const ::android::emulation::GfxStreamGraphicsAgentFactory agents;
    emugl::set_emugl_window_operations(*agents.android_get_QAndroidEmulatorWindowAgent());
    emugl::set_emugl_multi_display_operations(
        *agents.android_get_QAndroidMultiDisplayAgent());
}

} // namespace

struct AndroidHostIngress::State final {
    std::unique_ptr<::gfxstream::RendererImpl> renderer;
    std::unordered_map<uint32_t, AHardwareBuffer*> presentationTargets;
    std::uint32_t nextContextId{1};
    bool initialized{false};
};

AndroidHostIngress::AndroidHostIngress() : state_(std::make_unique<State>()) {}

AndroidHostIngress::~AndroidHostIngress() = default;

bool AndroidHostIngress::initialize(std::string& error) {
    error.clear();
    if (state_->initialized) return true;
    initializeHeadlessAgents();
    auto renderer = std::make_unique<::gfxstream::RendererImpl>();
    if (!renderer->initialize(1, 1, makeFeatures(), false, false)) {
        error = "could not initialize the headless gfxstream Vulkan renderer";
        return false;
    }
    state_->renderer = std::move(renderer);
    state_->initialized = true;
    std::cerr << "[LCL Gpud] gfxstream headless Vulkan decoder initialized\n";
    return true;
}

std::optional<AndroidHostIngress::ColorBufferTarget> AndroidHostIngress::createColorBuffer(
        const uint32_t width, const uint32_t height) {
    constexpr uint32_t kMaximumDimension = 16'384;
    if (!state_->initialized || width == 0 || height == 0 ||
        width > kMaximumDimension || height > kMaximumDimension) {
        return std::nullopt;
    }
    auto* frameBuffer = ::gfxstream::FrameBuffer::getFB();
    if (!frameBuffer) return std::nullopt;
    AHardwareBuffer_Desc description{};
    description.width = width;
    description.height = height;
    description.layers = 1;
    description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    description.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                        AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    AHardwareBuffer* hardwareBuffer = nullptr;
    if (AHardwareBuffer_allocate(&description, &hardwareBuffer) != 0 || !hardwareBuffer) {
        return std::nullopt;
    }
    AHardwareBuffer_describe(hardwareBuffer, &description);
    const auto handle = frameBuffer->createColorBuffer(
        static_cast<int>(width), static_cast<int>(height), GL_RGBA,
        ::gfxstream::FRAMEWORK_FORMAT_GL_COMPATIBLE);
    if (handle == 0 || frameBuffer->openColorBuffer(handle) != 0) {
        if (handle != 0) frameBuffer->closeColorBuffer(handle);
        AHardwareBuffer_release(hardwareBuffer);
        return std::nullopt;
    }
    // The upstream registry retains a separate reference for guest Vulkan
    // imports. LCL retains this one until rasterd returns the presentation
    // buffer, then destroys both references together.
    if (!::gfxstream::vk::registerAndroidHardwareBufferForColorBuffer(handle,
                                                                        hardwareBuffer)) {
        frameBuffer->closeColorBuffer(handle);
        AHardwareBuffer_release(hardwareBuffer);
        return std::nullopt;
    }
    state_->presentationTargets.emplace(handle, hardwareBuffer);
    return ColorBufferTarget{handle, description.stride};
}

bool AndroidHostIngress::deliverColorBuffer(const uint32_t colorBufferHandle,
                                             const int sidebandFd) noexcept {
    const auto found = state_->presentationTargets.find(colorBufferHandle);
    return sidebandFd >= 0 && found != state_->presentationTargets.end() &&
           AHardwareBuffer_sendHandleToUnixSocket(found->second, sidebandFd) == 0;
}

void AndroidHostIngress::destroyColorBuffer(const uint32_t colorBufferHandle) noexcept {
    if (!state_->initialized || colorBufferHandle == 0) return;
    const auto found = state_->presentationTargets.find(colorBufferHandle);
    if (found != state_->presentationTargets.end()) {
        ::gfxstream::vk::unregisterAndroidHardwareBufferForColorBuffer(colorBufferHandle);
        AHardwareBuffer_release(found->second);
        state_->presentationTargets.erase(found);
    }
    if (auto* frameBuffer = ::gfxstream::FrameBuffer::getFB()) {
        frameBuffer->closeColorBuffer(colorBufferHandle);
    }
}

void AndroidHostIngress::serve(const int socketDescriptor) noexcept {
    if (!state_->initialized || socketDescriptor < 0 || !setNonBlocking(socketDescriptor)) return;
    const std::uint32_t contextId = state_->nextContextId++;
    if (contextId == 0 || state_->nextContextId == 0) return;
    // The virtio frontend normally brackets each RenderChannel with this
    // upstream resource-owner registration.  LCL has no virtio frontend, but
    // the same context ownership is required for the decoder's disconnect
    // cleanup path.
    state_->renderer->onGuestGraphicsProcessCreate(contextId);
    auto channel = state_->renderer->createRenderChannel(nullptr, contextId);
    if (!channel) return;
    std::cerr << "[LCL Gpud] gfxstream RenderChannel created context=" << contextId << '\n';

    std::vector<std::uint8_t> pendingGuest;
    std::size_t guestOffset = 0;
    std::vector<std::uint8_t> pendingHost;
    std::size_t guestPacketCount = 0;
    std::size_t hostPacketCount = 0;
    const char* traceSetting = std::getenv("LCL_GPU_TRACE_PACKETS");
    const bool tracePackets = traceSetting && traceSetting[0] == '1';
    bool live = true;
    while (live) {
        while (guestOffset < pendingGuest.size()) {
            const std::size_t count = std::min<std::size_t>(
                ::gfxstream::RenderChannel::Buffer::kSmallSize, pendingGuest.size() - guestOffset);
            ::gfxstream::RenderChannel::Buffer input(
                reinterpret_cast<const char*>(pendingGuest.data() + guestOffset),
                reinterpret_cast<const char*>(pendingGuest.data() + guestOffset + count));
            const auto result = channel->tryWrite(std::move(input));
            if (result != ::gfxstream::RenderChannel::IoResult::Ok) break;
            guestOffset += count;
        }
        if (guestOffset == pendingGuest.size()) {
            pendingGuest.clear();
            guestOffset = 0;
        }

        while (pendingHost.empty()) {
            ::gfxstream::RenderChannel::Buffer output;
            const auto result = channel->tryRead(&output);
            if (result != ::gfxstream::RenderChannel::IoResult::Ok) break;
            pendingHost.assign(output.begin(), output.end());
        }
        if (!pendingHost.empty()) {
            if (sendPacket(socketDescriptor, pendingHost)) {
                ++hostPacketCount;
                if (tracePackets) {
                    std::cerr << "[LCL Gpud] gfxstream host bytes context=" << contextId
                              << " packet=" << hostPacketCount
                              << " size=" << pendingHost.size() << '\n';
                }
                pendingHost.clear();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                live = false;
            }
        }

        if ((channel->state() & ::gfxstream::RenderChannel::State::Stopped) !=
            ::gfxstream::RenderChannel::State::Empty) {
            break;
        }
        pollfd ready{socketDescriptor, static_cast<short>(pendingGuest.empty() ? POLLIN : 0), 0};
        int result = -1;
        do {
            result = poll(&ready, 1, kPumpTimeoutMilliseconds);
        } while (result < 0 && errno == EINTR);
        if (result < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;
        if (result > 0 && (ready.revents & POLLIN) != 0 &&
            !receivePacket(socketDescriptor, pendingGuest)) {
            break;
        }
        if (!pendingGuest.empty()) {
            ++guestPacketCount;
            if (tracePackets) {
                std::cerr << "[LCL Gpud] gfxstream guest bytes context=" << contextId
                          << " packet=" << guestPacketCount
                          << " size=" << pendingGuest.size() << '\n';
            }
        }
    }
    std::cerr << "[LCL Gpud] gfxstream context=" << contextId
              << " closed guest_packets=" << guestPacketCount
              << " host_packets=" << hostPacketCount << '\n';
    channel->stop();
    state_->renderer->cleanupProcGLObjects(contextId);
}

} // namespace lcl::gfxstream
