#include "lcl-gfxstream/android_host_ingress.hpp"

#include "RendererImpl.h"
#include "gfxstream/host/Features.h"
#include "render-utils/RenderChannel.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace lcl::gfxstream {
namespace {

constexpr std::size_t kMaximumSocketPacketBytes = 60 * 1024;
constexpr int kPumpTimeoutMilliseconds = 10;

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
    enable(features.HostComposition);
    enable(features.NoDelayCloseColorBuffer);
    enable(features.PlayStoreImage);
    enable(features.VirtioGpuFenceContexts);
    enable(features.VirtioGpuNativeSync);
    enable(features.VirtioGpuNext);
    enable(features.Vulkan);
    enable(features.VulkanBatchedDescriptorSetUpdate);
    enable(features.VulkanIgnoredHandles);
    enable(features.VulkanNullOptionalStrings);
    enable(features.VulkanQueueSubmitWithCommands);
    enable(features.VulkanShaderFloat16Int8);
    return features;
}

} // namespace

struct AndroidHostIngress::State final {
    std::unique_ptr<::gfxstream::RendererImpl> renderer;
    std::uint32_t nextContextId{1};
    bool initialized{false};
};

AndroidHostIngress::AndroidHostIngress() : state_(std::make_unique<State>()) {}

AndroidHostIngress::~AndroidHostIngress() = default;

bool AndroidHostIngress::initialize(std::string& error) {
    error.clear();
    if (state_->initialized) return true;
    auto renderer = std::make_unique<::gfxstream::RendererImpl>();
    if (!renderer->initialize(1, 1, makeFeatures(), false, false)) {
        error = "could not initialize the headless gfxstream Vulkan renderer";
        return false;
    }
    state_->renderer = std::move(renderer);
    state_->initialized = true;
    return true;
}

void AndroidHostIngress::serve(const int socketDescriptor) noexcept {
    if (!state_->initialized || socketDescriptor < 0 || !setNonBlocking(socketDescriptor)) return;
    const std::uint32_t contextId = state_->nextContextId++;
    if (contextId == 0 || state_->nextContextId == 0) return;
    auto channel = state_->renderer->createRenderChannel(nullptr, contextId);
    if (!channel) return;

    std::vector<std::uint8_t> pendingGuest;
    std::size_t guestOffset = 0;
    std::vector<std::uint8_t> pendingHost;
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
    }
    channel->stop();
    state_->renderer->cleanupProcGLObjects(contextId);
}

} // namespace lcl::gfxstream
