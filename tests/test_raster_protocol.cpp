#include <gtest/gtest.h>

#include "core/ipc/raster_protocol.hpp"
#include "render/retained_output_damage.hpp"

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace lcl::raster_protocol {

namespace {

lcl::render::RetainedOutputDamageTracker::Frame outputFrame(
        uint64_t serial, uint64_t base, lcl::render::RasterRect damage,
        bool replacesScene = false) {
    return {serial, base, 7, 100, 200, 2.0f, damage, replacesScene};
}

} // namespace

TEST(RasterProtocolTest, LayerReadyCarriesGenerationAndOnePrivateDescriptor) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    const int descriptor = dup(STDIN_FILENO);
    ASSERT_GE(descriptor, 0);

    LayerReady sent{};
    sent.grant = {7, 42, kGrantInteractiveSystem, 11, 13};
    sent.layerId = 99;
    sent.bufferId = 101;
    sent.configureSerial = 17;
    sent.frameSerial = 19;
    sent.geometryGeneration = 23;
    sent.width = 640;
    sent.height = 480;
    sent.backingWidth = 672;
    sent.backingHeight = 512;
    sent.stride = 2560;
    sent.damageWidth = 640;
    sent.damageHeight = 480;
    sent.transport = LayerTransport::DmaBuf;
    sent.format = 0x34325241u;
    sent.modifier = 7;
    sent.byteSize = 640u * 480u * sizeof(uint32_t);
    ASSERT_TRUE(sendPacket(sockets[0], Opcode::LayerReady, sent, descriptor));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Received);
    const auto* ready = payloadAs<LayerReady>(header, payload, Opcode::LayerReady);
    ASSERT_NE(ready, nullptr);
    EXPECT_EQ(ready->grant.tokenHigh, 11u);
    EXPECT_EQ(ready->grant.tokenLow, 13u);
    EXPECT_EQ(ready->bufferId, 101u);
    EXPECT_EQ(ready->configureSerial, 17u);
    EXPECT_EQ(ready->frameSerial, 19u);
    EXPECT_EQ(ready->geometryGeneration, 23u);
    EXPECT_EQ(ready->backingWidth, 672u);
    EXPECT_EQ(ready->backingHeight, 512u);
    EXPECT_EQ(ready->transport, LayerTransport::DmaBuf);
    EXPECT_EQ(ready->format, 0x34325241u);
    EXPECT_EQ(ready->modifier, 7u);
    EXPECT_GE(receivedFd, 0);

    close(receivedFd);
    close(descriptor);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, CommitTransactionCarriesMutationsAndLogicalDamage) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    const int descriptor = dup(STDIN_FILENO);
    ASSERT_GE(descriptor, 0);

    CommitTransaction sent{};
    sent.grant = {7, 42, 0, 11, 13};
    sent.configureSerial = 17;
    sent.frameSerial = 23;
    sent.baseFrameSerial = 19;
    sent.geometryGeneration = 29;
    sent.logicalWidth = 640.0f;
    sent.logicalHeight = 480.0f;
    sent.bufferScale = 1.5f;
    sent.damageX = 20.0f;
    sent.damageY = 30.0f;
    sent.damageWidth = 40.0f;
    sent.damageHeight = 50.0f;
    sent.displayListSize = 4096;
    sent.mutationCount = 2;
    std::vector<NodeMutation> sentMutations(2);
    sentMutations[0].type = NodeMutationType::CreateNode;
    sentMutations[0].node.id = 71;
    sentMutations[0].node.contentRevision = 3;
    sentMutations[0].node.propertyRevision = 5;
    sentMutations[0].node.boundaryReasons = 1;
    sentMutations[1].type = NodeMutationType::SetProperties;
    sentMutations[1].node.id = 73;
    sentMutations[1].node.parentId = 71;
    sentMutations[1].node.contentRevision = 7;
    sentMutations[1].node.propertyRevision = 11;
    sentMutations[1].node.boundaryReasons = 16;
    sentMutations[1].node.translationY = -40.0f;
    ASSERT_TRUE(sendCommitTransaction(
        sockets[0], sent, sentMutations, descriptor));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Received);
    CommitTransaction received{};
    std::vector<NodeMutation> receivedMutations;
    ASSERT_TRUE(decodeCommitTransaction(
        header, payload, received, receivedMutations));
    EXPECT_EQ(received.baseFrameSerial, 19u);
    EXPECT_FLOAT_EQ(received.damageX, 20.0f);
    EXPECT_FLOAT_EQ(received.damageY, 30.0f);
    EXPECT_FLOAT_EQ(received.damageWidth, 40.0f);
    EXPECT_FLOAT_EQ(received.damageHeight, 50.0f);
    EXPECT_EQ(received.flags, 0u);
    ASSERT_EQ(receivedMutations.size(), 2u);
    EXPECT_EQ(receivedMutations[0].type, NodeMutationType::CreateNode);
    EXPECT_EQ(receivedMutations[0].node.id, 71u);
    EXPECT_EQ(receivedMutations[1].type, NodeMutationType::SetProperties);
    EXPECT_FLOAT_EQ(receivedMutations[1].node.translationY, -40.0f);
    EXPECT_GE(receivedFd, 0);

    close(receivedFd);
    close(descriptor);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, RejectsTransactionWithMismatchedMutationCount) {
    Header header{};
    header.opcode = Opcode::CommitTransaction;
    CommitTransaction transaction{};
    transaction.mutationCount = 1;
    header.payloadSize = sizeof(transaction);
    std::vector<uint8_t> payload(sizeof(transaction));
    std::memcpy(payload.data(), &transaction, sizeof(transaction));

    CommitTransaction decoded{};
    std::vector<NodeMutation> mutations;
    EXPECT_FALSE(decodeCommitTransaction(
        header, payload, decoded, mutations));
    EXPECT_TRUE(mutations.empty());
}

TEST(RasterProtocolTest, PropertyOnlyTransactionCarriesNoDisplayListDescriptor) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);

    CommitTransaction sent{};
    sent.grant = {7, 42, 0, 11, 13};
    sent.configureSerial = 17;
    sent.frameSerial = 23;
    sent.baseFrameSerial = 19;
    sent.geometryGeneration = 29;
    sent.logicalWidth = 640.0f;
    sent.logicalHeight = 480.0f;
    sent.bufferScale = 1.0f;
    sent.damageWidth = 640.0f;
    sent.damageHeight = 100.0f;
    sent.mutationCount = 1;
    NodeMutation mutation{};
    mutation.type = NodeMutationType::SetProperties;
    mutation.node.id = 73;
    mutation.node.parentId = 71;
    mutation.node.contentRevision = 7;
    mutation.node.boundaryReasons = 1u << 5u;
    mutation.node.layoutWidth = 640.0f;
    mutation.node.layoutHeight = 1200.0f;
    mutation.node.presentationWidth = 640.0f;
    mutation.node.presentationHeight = 1200.0f;
    mutation.node.translationY = -40.0f;
    ASSERT_TRUE(sendCommitTransaction(
        sockets[0], sent, std::span<const NodeMutation>(&mutation, 1), -1));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Received);
    CommitTransaction received{};
    std::vector<NodeMutation> mutations;
    ASSERT_TRUE(decodeCommitTransaction(
        header, payload, received, mutations));
    EXPECT_EQ(received.displayListSize, 0u);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_FLOAT_EQ(mutations[0].node.translationY, -40.0f);
    EXPECT_EQ(receivedFd, -1);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, CommitDescriptorMustMatchDisplayListPresence) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    const int descriptor = dup(STDIN_FILENO);
    ASSERT_GE(descriptor, 0);

    CommitTransaction transaction{};
    EXPECT_FALSE(sendCommitTransaction(
        sockets[0], transaction, std::span<const NodeMutation>{}, descriptor));
    transaction.displayListSize = 1;
    EXPECT_FALSE(sendCommitTransaction(
        sockets[0], transaction, std::span<const NodeMutation>{}, -1));

    close(descriptor);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, RejectsWrongPrivateProtocolVersion) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    Header invalid{};
    invalid.version = kVersion + 1;
    ASSERT_EQ(send(sockets[0], &invalid, sizeof(invalid), MSG_NOSIGNAL),
              static_cast<ssize_t>(sizeof(invalid)));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    EXPECT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Invalid);
    EXPECT_EQ(receivedFd, -1);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, ReleaseLayerCarriesTransportRejectionReason) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);

    ReleaseLayer sent{};
    sent.layerId = 73;
    sent.reason = LayerReleaseReason::RejectedTransport;
    ASSERT_TRUE(sendPacket(sockets[0], Opcode::ReleaseLayer, sent));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Received);
    const auto* received = payloadAs<ReleaseLayer>(
        header, payload, Opcode::ReleaseLayer);
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->layerId, 73u);
    EXPECT_EQ(received->reason, LayerReleaseReason::RejectedTransport);
    EXPECT_EQ(receivedFd, -1);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, RotatingOutputCopiesOnlyDamageMissedByReusedBuffer) {
    lcl::render::RetainedOutputDamageTracker tracker;
    const auto first = outputFrame(
        1, 0, {0.0f, 0.0f, 50.0f, 100.0f}, true);
    EXPECT_FALSE(tracker.copyDamage(11, first).has_value());
    tracker.commit(11, first);

    const auto second = outputFrame(
        2, 1, {4.0f, 10.0f, 8.0f, 12.0f});
    EXPECT_FALSE(tracker.copyDamage(12, second).has_value());
    tracker.commit(12, second);

    const auto third = outputFrame(
        3, 2, {20.0f, 6.0f, 5.0f, 8.0f});
    const auto copied = tracker.copyDamage(11, third);
    ASSERT_TRUE(copied.has_value());
    EXPECT_FLOAT_EQ(copied->x, 4.0f);
    EXPECT_FLOAT_EQ(copied->y, 6.0f);
    EXPECT_FLOAT_EQ(copied->width, 21.0f);
    EXPECT_FLOAT_EQ(copied->height, 16.0f);
}

TEST(RasterProtocolTest, RotatingOutputFallsBackToFullCopyAcrossReplacement) {
    lcl::render::RetainedOutputDamageTracker tracker;
    tracker.commit(21, outputFrame(
        7, 0, {0.0f, 0.0f, 50.0f, 100.0f}, true));

    // A new producer lifecycle may restart frame serials. The replacement
    // invalidates every other slot even when its stale serial happens to
    // equal the new retained base.
    const auto replacement = outputFrame(
        7, 0, {0.0f, 0.0f, 50.0f, 100.0f}, true);
    tracker.commit(22, replacement);

    const auto patch = outputFrame(
        8, 7, {10.0f, 10.0f, 3.0f, 3.0f});
    EXPECT_FALSE(tracker.copyDamage(21, patch).has_value());
}

} // namespace lcl::raster_protocol
