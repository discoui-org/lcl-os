#include "system/ipc/raster_protocol.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <sys/socket.h>
#include <unistd.h>

namespace lcl::raster_protocol {

bool sendPacket(int fd, Opcode opcode, const void* payload,
                uint32_t payloadSize, int passedFd) {
    if (fd < 0 || payloadSize > kMaxPayload || (payloadSize != 0 && !payload)) {
        return false;
    }

    Header header{};
    header.opcode = opcode;
    header.payloadSize = payloadSize;

    iovec vectors[2]{};
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = const_cast<void*>(payload);
    vectors[1].iov_len = payloadSize;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = vectors;
    message.msg_iovlen = payloadSize == 0 ? 1 : 2;
    if (passedFd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        auto* cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &passedFd, sizeof(int));
    }

    const auto expected = static_cast<ssize_t>(sizeof(header) + payloadSize);
    ssize_t sent = -1;
    do {
        sent = sendmsg(fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == expected;
}

bool sendCommitTransaction(
        int fd, const CommitTransaction& transaction,
        std::span<const NodeMutation> mutations, int displayListFd) {
    const bool hasDisplayList = transaction.displayListSize != 0;
    if (mutations.size() != transaction.mutationCount ||
        hasDisplayList != (displayListFd >= 0) ||
        mutations.size() >
            (kMaxPayload - sizeof(CommitTransaction)) / sizeof(NodeMutation)) {
        return false;
    }
    const size_t payloadSize = sizeof(CommitTransaction) +
        mutations.size() * sizeof(NodeMutation);
    if (payloadSize > std::numeric_limits<uint32_t>::max()) return false;
    std::vector<uint8_t> payload(payloadSize);
    std::memcpy(payload.data(), &transaction, sizeof(transaction));
    if (!mutations.empty()) {
        std::memcpy(payload.data() + sizeof(transaction), mutations.data(),
                    mutations.size_bytes());
    }
    return sendPacket(fd, Opcode::CommitTransaction, payload.data(),
                      static_cast<uint32_t>(payload.size()), displayListFd);
}

bool decodeCommitTransaction(
        const Header& header, std::span<const uint8_t> payload,
        CommitTransaction& transaction,
        std::vector<NodeMutation>& mutations) {
    mutations.clear();
    if (header.opcode != Opcode::CommitTransaction ||
        header.payloadSize != payload.size() ||
        payload.size() < sizeof(CommitTransaction)) {
        return false;
    }
    std::memcpy(&transaction, payload.data(), sizeof(transaction));
    const size_t available = payload.size() - sizeof(CommitTransaction);
    if (transaction.mutationCount >
            available / sizeof(NodeMutation) ||
        available != static_cast<size_t>(transaction.mutationCount) *
            sizeof(NodeMutation)) {
        return false;
    }
    mutations.resize(transaction.mutationCount);
    if (!mutations.empty()) {
        std::memcpy(mutations.data(), payload.data() + sizeof(transaction),
                    mutations.size() * sizeof(NodeMutation));
    }
    return true;
}

namespace {

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool validPresentationLayer(const PresentationLayerState& layer) noexcept {
    constexpr uint32_t kAllowedFlags =
        kPresentationLayerHasClip | kPresentationLayerOpaque;
    if (layer.nodeId == 0 || layer.layerId == 0 ||
        layer.contentRevision == 0 || (layer.flags & ~kAllowedFlags) != 0 ||
        !finite(layer.x) || !finite(layer.y) ||
        !finite(layer.width) || layer.width <= 0.0f ||
        !finite(layer.height) || layer.height <= 0.0f ||
        !finite(layer.opacity) || layer.opacity < 0.0f || layer.opacity > 1.0f ||
        !finite(layer.translationX) || !finite(layer.translationY) ||
        !finite(layer.scaleX) || layer.scaleX <= 0.0f ||
        !finite(layer.scaleY) || layer.scaleY <= 0.0f ||
        !finite(layer.rotationRadians) ||
        !finite(layer.originX) || layer.originX < 0.0f || layer.originX > 1.0f ||
        !finite(layer.originY) || layer.originY < 0.0f || layer.originY > 1.0f) {
        return false;
    }
    const bool hasClip = (layer.flags & kPresentationLayerHasClip) != 0;
    return !hasClip ||
        (finite(layer.clipX) && finite(layer.clipY) &&
         finite(layer.clipWidth) && layer.clipWidth > 0.0f &&
         finite(layer.clipHeight) && layer.clipHeight > 0.0f);
}

bool validPresentationFrame(
        const PresentationFrameReady& frame,
        std::span<const PresentationLayerState> layers) noexcept {
    if (frame.grant.surfaceId == 0 || frame.configureSerial == 0 ||
        frame.frameSerial == 0 || frame.rootNodeId == 0 ||
        frame.reserved != 0 || frame.layerCount == 0 ||
        frame.layerCount != layers.size() ||
        layers.size() >
            (kMaxPayload - sizeof(PresentationFrameReady)) /
                sizeof(PresentationLayerState)) {
        return false;
    }
    std::unordered_set<uint64_t> nodes;
    std::unordered_set<uint64_t> layerIds;
    nodes.reserve(layers.size());
    layerIds.reserve(layers.size());
    bool containsRoot = false;
    for (const auto& layer : layers) {
        if (!validPresentationLayer(layer) ||
            !nodes.insert(layer.nodeId).second ||
            !layerIds.insert(layer.layerId).second) {
            return false;
        }
        containsRoot = containsRoot || layer.nodeId == frame.rootNodeId;
    }
    return containsRoot;
}

bool validPresentationAnimation(
        const PresentationAnimation& animation) noexcept {
    constexpr uint32_t kAllowedProperties =
        kAnimationTranslation | kAnimationScale |
        kAnimationRotation | kAnimationOpacity;
    const bool finiteValues = finite(animation.durationSec) &&
        finite(animation.initialVelocityX) && finite(animation.initialVelocityY) &&
        finite(animation.initialVelocityScale) &&
        finite(animation.initialVelocityRotation) &&
        finite(animation.initialVelocityOpacity) &&
        finite(animation.startTranslationX) && finite(animation.startTranslationY) &&
        finite(animation.startScaleX) && finite(animation.startScaleY) &&
        finite(animation.startRotationRadians) && finite(animation.startOpacity) &&
        finite(animation.targetTranslationX) && finite(animation.targetTranslationY) &&
        finite(animation.targetScaleX) && finite(animation.targetScaleY) &&
        finite(animation.targetRotationRadians) && finite(animation.targetOpacity) &&
        finite(animation.springMass) && finite(animation.springStiffness) &&
        finite(animation.springDamping);
    if (animation.grant.surfaceId == 0 || animation.transactionId == 0 ||
        animation.nodeId == 0 || animation.propertyMask == 0 ||
        (animation.propertyMask & ~kAllowedProperties) != 0 ||
        !finiteValues || animation.durationSec < 0.0f ||
        animation.durationSec > 10.0f || animation.startScaleX <= 0.0f ||
        animation.startScaleY <= 0.0f || animation.targetScaleX <= 0.0f ||
        animation.targetScaleY <= 0.0f || animation.startOpacity < 0.0f ||
        animation.startOpacity > 1.0f || animation.targetOpacity < 0.0f ||
        animation.targetOpacity > 1.0f) {
        return false;
    }
    if (animation.curve == PresentationAnimationCurve::Tween) {
        return animation.durationSec > 0.0f;
    }
    return animation.curve == PresentationAnimationCurve::Spring &&
        animation.springMass > 0.0f && animation.springStiffness > 0.0f &&
        animation.springDamping >= 0.0f;
}

} // namespace

bool sendPresentationFrameReady(
        int fd, const PresentationFrameReady& frame,
        std::span<const PresentationLayerState> layers) {
    if (!validPresentationFrame(frame, layers)) return false;
    const size_t payloadSize = sizeof(PresentationFrameReady) +
        layers.size_bytes();
    if (payloadSize > std::numeric_limits<uint32_t>::max()) return false;
    std::vector<uint8_t> payload(payloadSize);
    std::memcpy(payload.data(), &frame, sizeof(frame));
    std::memcpy(payload.data() + sizeof(frame), layers.data(),
                layers.size_bytes());
    return sendPacket(fd, Opcode::PresentationFrameReady,
                      static_cast<const void*>(payload.data()),
                      static_cast<uint32_t>(payload.size()), -1);
}

bool decodePresentationFrameReady(
        const Header& header, std::span<const uint8_t> payload,
        PresentationFrameReady& frame,
        std::vector<PresentationLayerState>& layers) {
    layers.clear();
    if (header.opcode != Opcode::PresentationFrameReady ||
        header.payloadSize != payload.size() ||
        payload.size() < sizeof(PresentationFrameReady)) {
        return false;
    }
    std::memcpy(&frame, payload.data(), sizeof(frame));
    const size_t available = payload.size() - sizeof(frame);
    if (frame.layerCount > available / sizeof(PresentationLayerState) ||
        available != static_cast<size_t>(frame.layerCount) *
            sizeof(PresentationLayerState)) {
        return false;
    }
    layers.resize(frame.layerCount);
    if (!layers.empty()) {
        std::memcpy(layers.data(), payload.data() + sizeof(frame),
                    layers.size() * sizeof(PresentationLayerState));
    }
    if (!validPresentationFrame(frame, layers)) {
        layers.clear();
        return false;
    }
    return true;
}

bool sendPresentationAnimation(
        int fd, const PresentationAnimation& animation) {
    return validPresentationAnimation(animation) &&
        sendPacket(fd, Opcode::PresentationAnimation, animation);
}

bool decodePresentationAnimation(
        const Header& header, std::span<const uint8_t> payload,
        PresentationAnimation& animation) {
    const auto* decoded = payloadAs<PresentationAnimation>(
        header, payload, Opcode::PresentationAnimation);
    if (!decoded || !validPresentationAnimation(*decoded)) return false;
    animation = *decoded;
    return true;
}

ReceiveStatus receivePacket(int fd, Header& header,
                            std::vector<uint8_t>& payload, int& receivedFd,
                            SenderCredentials* sender) {
    // receivePacket() is called from non-blocking drain loops, so the common
    // case is WouldBlock. Allocating and zero-filling kMaxPayload before every
    // recvmsg made an idle raster service churn through hundreds of MiB/s.
    // Keep one uninitialised receive slab per polling thread and copy only the
    // bytes that were actually delivered into the caller-owned payload.
    thread_local std::array<uint8_t, kMaxPayload> receivePayload{};
    payload.clear();
    receivedFd = -1;
    if (sender) *sender = {};

    iovec vectors[2]{};
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = receivePayload.data();
    vectors[1].iov_len = receivePayload.size();

    alignas(cmsghdr) char control[
        CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(ucred))]{};
    msghdr message{};
    message.msg_iov = vectors;
    message.msg_iovlen = 2;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    ssize_t received = -1;
    do {
        received = recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK
            ? ReceiveStatus::WouldBlock : ReceiveStatus::Error;
    }
    if (received == 0) {
        return ReceiveStatus::Closed;
    }

    bool hasCredentials = false;
    bool validAncillary = true;
    for (auto* cmsg = CMSG_FIRSTHDR(&message); cmsg;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
                int descriptor = -1;
                std::memcpy(&descriptor, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                if (receivedFd < 0) receivedFd = descriptor;
                else {
                    close(descriptor);
                    validAncillary = false;
                }
            }
        } else if (cmsg->cmsg_level == SOL_SOCKET &&
                   cmsg->cmsg_type == SCM_CREDENTIALS) {
            if (hasCredentials || cmsg->cmsg_len != CMSG_LEN(sizeof(ucred))) {
                validAncillary = false;
                continue;
            }
            ucred credentials{};
            std::memcpy(&credentials, CMSG_DATA(cmsg), sizeof(credentials));
            hasCredentials = credentials.pid > 0;
            if (sender) *sender = {credentials.pid, credentials.uid, credentials.gid};
        }
    }

    const bool validHeader = validAncillary && (!sender || hasCredentials) &&
        received >= static_cast<ssize_t>(sizeof(Header)) &&
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 &&
        header.magic == kMagic && header.version == kVersion &&
        header.payloadSize <= kMaxPayload &&
        received == static_cast<ssize_t>(sizeof(Header) + header.payloadSize);
    if (!validHeader) {
        if (receivedFd >= 0) close(receivedFd);
        receivedFd = -1;
        if (sender) *sender = {};
        return ReceiveStatus::Invalid;
    }

    payload.assign(receivePayload.begin(),
                   receivePayload.begin() + header.payloadSize);
    return ReceiveStatus::Received;
}

} // namespace lcl::raster_protocol
