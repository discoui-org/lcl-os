#include <gtest/gtest.h>

#include "lcl-gpu/gpu_client.hpp"
#include "lcl-gpu/gfxstream_packet_stream.hpp"
#include "lcl-gpu/gl_presentation.hpp"
#include "system/ipc/gpu_protocol.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace lcl::gpu {
namespace {

TEST(GlPresentationTest, CreatesNonOwningTargetsWithoutPlatformBranching) {
    auto presentation = createGlPresentation();
    ASSERT_NE(presentation, nullptr);
    GlPresentTarget target{};
    std::string error;
    EXPECT_FALSE(presentation->createTarget(0, 0, 32, target, error));
    EXPECT_EQ(target.bufferId, 0u);
    ASSERT_TRUE(presentation->createTarget(7, 64, 32, target, error)) << error;
    EXPECT_NE(target.bufferId, 0u);
    EXPECT_EQ(target.framebuffer, 7u);
    EXPECT_EQ(target.width, 64u);
    EXPECT_EQ(target.height, 32u);
    presentation->destroyTarget(target);
    EXPECT_EQ(target.bufferId, 0u);
    EXPECT_EQ(target.framebuffer, 0u);
}

class GpuClientTest : public ::testing::Test {
protected:
    std::string socketPath_;
    int listener_{-1};

    void SetUp() override {
        socketPath_ = (std::filesystem::temp_directory_path() /
                       ("lcl_gpu_client_" + std::to_string(getpid()) + ".sock"))
                          .string();
        listener_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        ASSERT_GE(listener_, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socketPath_.c_str(), sizeof(address.sun_path) - 1);
        ASSERT_EQ(bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
        ASSERT_EQ(listen(listener_, 1), 0);
    }

    void TearDown() override {
        if (listener_ >= 0) close(listener_);
        unlink(socketPath_.c_str());
    }

    int acceptClient() {
        const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        EXPECT_GE(client, 0);
        return client;
    }
};

TEST_F(GpuClientTest, ReceivesADeviceCapabilityReply) {
    GpuClient client;
    ASSERT_TRUE(client.connect(socketPath_));
    const int server = acceptClient();
    ASSERT_GE(server, 0);
    ASSERT_TRUE(client.beginHandshake(123));

    lcl::gpu_protocol::Header request{};
    std::vector<uint8_t> requestPayload;
    ASSERT_EQ(lcl::gpu_protocol::receivePacket(server, request, requestPayload),
              lcl::gpu_protocol::ReceiveStatus::Received);
    const auto* hello = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::Hello>(
        request, requestPayload, lcl::gpu_protocol::Opcode::Hello);
    ASSERT_NE(hello, nullptr);
    EXPECT_EQ(hello->nonce, 123u);

    lcl::gpu_protocol::DeviceInfo reply{};
    reply.vulkanApiVersion = (1u << 22) | (1u << 12);
    reply.vendorId = 42;
    reply.maxImageDimension2D = 8192;
    reply.flags = lcl::gpu_protocol::kDeviceSupportsAndroidHardwareBuffer |
                  lcl::gpu_protocol::kDeviceSupportsExternalFenceFd;
    std::strncpy(reply.deviceName, "LCL test GPU", sizeof(reply.deviceName) - 1);
    ASSERT_TRUE(lcl::gpu_protocol::sendPacket(
        server, lcl::gpu_protocol::Opcode::DeviceInfo, &reply, sizeof(reply)));

    pollfd ready{client.fd(), POLLIN, 0};
    ASSERT_EQ(poll(&ready, 1, 1'000), 1);
    DeviceCapabilities capabilities{};
    ASSERT_EQ(client.dispatch(capabilities), HandshakeStatus::Ready);
    EXPECT_EQ(capabilities.vendorId, 42u);
    EXPECT_EQ(capabilities.maxImageDimension2D, 8192u);
    EXPECT_TRUE(capabilities.supportsAndroidHardwareBuffer);
    EXPECT_TRUE(capabilities.supportsExternalFenceFd);
    EXPECT_EQ(capabilities.deviceName, "LCL test GPU");
    close(server);
}

TEST_F(GpuClientTest, RejectsUnexpectedReply) {
    GpuClient client;
    ASSERT_TRUE(client.connect(socketPath_));
    const int server = acceptClient();
    ASSERT_GE(server, 0);
    ASSERT_TRUE(client.beginHandshake(123));

    lcl::gpu_protocol::Error reply{};
    ASSERT_TRUE(lcl::gpu_protocol::sendPacket(
        server, lcl::gpu_protocol::Opcode::Error, &reply, sizeof(reply)));
    pollfd ready{client.fd(), POLLIN, 0};
    ASSERT_EQ(poll(&ready, 1, 1'000), 1);
    DeviceCapabilities capabilities{};
    EXPECT_EQ(client.dispatch(capabilities), HandshakeStatus::ProtocolError);
    EXPECT_FALSE(client.connected());
    close(server);
}

TEST_F(GpuClientTest, DeliversBufferAndConsumesBothFenceOwnerships) {
    GpuClient client;
    ASSERT_TRUE(client.connect(socketPath_));
    const int server = acceptClient();
    ASSERT_GE(server, 0);
    ASSERT_TRUE(client.beginHandshake(77));

    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    ASSERT_EQ(lcl::gpu_protocol::receivePacket(server, header, payload),
              lcl::gpu_protocol::ReceiveStatus::Received);
    lcl::gpu_protocol::DeviceInfo device{};
    device.maxImageDimension2D = 4096;
    device.flags = lcl::gpu_protocol::kDeviceSupportsAndroidHardwareBuffer |
                   lcl::gpu_protocol::kDeviceSupportsExternalFenceFd;
    ASSERT_TRUE(lcl::gpu_protocol::sendPacket(
        server, lcl::gpu_protocol::Opcode::DeviceInfo, &device, sizeof(device)));
    pollfd ready{client.fd(), POLLIN, 0};
    ASSERT_EQ(poll(&ready, 1, 1'000), 1);
    DeviceCapabilities capabilities{};
    ASSERT_EQ(client.dispatch(capabilities), HandshakeStatus::Ready);

    std::thread broker([&] {
        const auto receiveWhenReadable = [&](lcl::gpu_protocol::Header& request,
                                             std::vector<uint8_t>& requestPayload,
                                             int& descriptor) {
            pollfd pending{server, POLLIN, 0};
            EXPECT_EQ(poll(&pending, 1, 1'000), 1);
            return lcl::gpu_protocol::receivePacketWithFd(
                server, request, requestPayload, descriptor);
        };
        lcl::gpu_protocol::Header request{};
        std::vector<uint8_t> requestPayload;
        int descriptor = -1;
        EXPECT_EQ(receiveWhenReadable(request, requestPayload, descriptor),
                  lcl::gpu_protocol::ReceiveStatus::Received);
        EXPECT_EQ(descriptor, -1);
        const auto* clear = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::ClearColor>(
            request, requestPayload, lcl::gpu_protocol::Opcode::ClearColor);
        ASSERT_NE(clear, nullptr);
        const int acquireFence = open("/dev/null", O_RDONLY | O_CLOEXEC);
        ASSERT_GE(acquireFence, 0);
        lcl::gpu_protocol::ClearColorReady cleared{};
        cleared.requestId = clear->requestId;
        cleared.bufferId = 91;
        cleared.width = clear->width;
        cleared.height = clear->height;
        cleared.stride = clear->width * sizeof(uint32_t);
        cleared.format = clear->format;
        ASSERT_TRUE(lcl::gpu_protocol::sendPacketWithFd(
            server, lcl::gpu_protocol::Opcode::ClearColorReady, &cleared,
            sizeof(cleared), acquireFence));
        close(acquireFence);

        request = {};
        requestPayload.clear();
        EXPECT_EQ(receiveWhenReadable(request, requestPayload, descriptor),
                  lcl::gpu_protocol::ReceiveStatus::Received);
        ASSERT_GE(descriptor, 0);
        const auto* delivery = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::DeliverNativeBuffer>(
            request, requestPayload, lcl::gpu_protocol::Opcode::DeliverNativeBuffer);
        ASSERT_NE(delivery, nullptr);
        EXPECT_EQ(delivery->bufferId, 91u);
        close(descriptor);
        descriptor = -1;
        const lcl::gpu_protocol::DeliveryComplete complete{91, 0, 0};
        ASSERT_TRUE(lcl::gpu_protocol::sendPacket(
            server, lcl::gpu_protocol::Opcode::DeliveryComplete,
            &complete, sizeof(complete)));

        request = {};
        requestPayload.clear();
        EXPECT_EQ(receiveWhenReadable(request, requestPayload, descriptor),
                  lcl::gpu_protocol::ReceiveStatus::Received);
        ASSERT_GE(descriptor, 0);
        const auto* release = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::ReleasePresentedBuffer>(
            request, requestPayload, lcl::gpu_protocol::Opcode::ReleasePresentedBuffer);
        ASSERT_NE(release, nullptr);
        EXPECT_EQ(release->bufferId, 91u);
        close(descriptor);
    });

    ClearedNativeBuffer buffer{};
    ASSERT_TRUE(client.clearColor(18, 64, 32,
                                  lcl::gpu_protocol::kAndroidHardwareBufferRgba8888,
                                  lcl::gpu_protocol::kAndroidHardwareBufferGpuSampledColorOutput,
                                  0.1f, 0.2f, 0.3f, 1.0f, buffer));
    ASSERT_TRUE(buffer.acquireFence);
    const int sideband = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(sideband, 0);
    ASSERT_TRUE(client.deliverNativeBuffer(buffer.bufferId, sideband));
    EXPECT_GE(fcntl(sideband, F_GETFD), 0); // SCM_RIGHTS sent a duplicate.
    close(sideband);

    const int releaseFence = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(releaseFence, 0);
    ASSERT_TRUE(client.releasePresentedBuffer(
        buffer.bufferId, client::OwnedFd(releaseFence)));
    broker.join();
    close(server);
}

TEST(GfxstreamPacketStreamTest, PreservesByteStreamAcrossPacketBoundaries) {
    int descriptors[2]{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors), 0);
    GfxstreamPacketStream sender{client::OwnedFd(descriptors[0])};
    GfxstreamPacketStream receiver{client::OwnedFd(descriptors[1])};

    std::vector<std::uint8_t> sent(GfxstreamPacketStream::kMaximumPacketBytes + 257);
    for (std::size_t index = 0; index < sent.size(); ++index) {
        sent[index] = static_cast<std::uint8_t>(index & 0xffu);
    }
    ASSERT_TRUE(sender.writeFully(sent.data(), sent.size()));
    std::vector<std::uint8_t> received(sent.size());
    ASSERT_TRUE(receiver.readFully(received.data(), received.size()));
    EXPECT_EQ(received, sent);

    constexpr std::array<std::uint8_t, 4> reply{0x46, 0x58, 0x53, 0x54};
    ASSERT_TRUE(receiver.writeFully(reply.data(), reply.size()));
    std::array<std::uint8_t, reply.size()> observed{};
    ASSERT_TRUE(sender.readFully(observed.data(), observed.size()));
    EXPECT_EQ(observed, reply);
}

TEST_F(GpuClientTest, OpensOpaqueGfxstreamStreamOnlyAfterHandshake) {
    GpuClient client;
    ASSERT_TRUE(client.connect(socketPath_));
    const int server = acceptClient();
    ASSERT_GE(server, 0);
    ASSERT_TRUE(client.beginHandshake(99));

    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    ASSERT_EQ(lcl::gpu_protocol::receivePacket(server, header, payload),
              lcl::gpu_protocol::ReceiveStatus::Received);
    lcl::gpu_protocol::DeviceInfo device{};
    device.maxImageDimension2D = 4096;
    ASSERT_TRUE(lcl::gpu_protocol::sendPacket(
        server, lcl::gpu_protocol::Opcode::DeviceInfo, &device, sizeof(device)));
    pollfd ready{client.fd(), POLLIN, 0};
    ASSERT_EQ(poll(&ready, 1, 1'000), 1);
    DeviceCapabilities capabilities{};
    ASSERT_EQ(client.dispatch(capabilities), HandshakeStatus::Ready);

    std::thread broker([&] {
        lcl::gpu_protocol::Header request{};
        std::vector<uint8_t> requestPayload;
        EXPECT_EQ(lcl::gpu_protocol::receivePacket(server, request, requestPayload),
                  lcl::gpu_protocol::ReceiveStatus::Received);
        const auto* open = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::OpenGfxstreamStream>(
            request, requestPayload, lcl::gpu_protocol::Opcode::OpenGfxstreamStream);
        ASSERT_NE(open, nullptr);
        const lcl::gpu_protocol::GfxstreamStreamReady accepted{};
        ASSERT_TRUE(lcl::gpu_protocol::sendPacket(
            server, lcl::gpu_protocol::Opcode::GfxstreamStreamReady,
            &accepted, sizeof(accepted)));
        std::array<std::uint8_t, 2> bytes{};
        ASSERT_EQ(recv(server, bytes.data(), bytes.size(), 0),
                  static_cast<ssize_t>(bytes.size()));
        EXPECT_EQ(bytes, (std::array<std::uint8_t, 2>{0xca, 0xfe}));
    });

    auto stream = client.openGfxstreamStream();
    ASSERT_TRUE(stream.has_value());
    EXPECT_FALSE(client.connected());
    constexpr std::array<std::uint8_t, 2> command{0xca, 0xfe};
    ASSERT_TRUE(stream->writeFully(command.data(), command.size()));
    broker.join();
    close(server);
}

} // namespace
} // namespace lcl::gpu
