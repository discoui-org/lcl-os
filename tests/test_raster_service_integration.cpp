#include <gtest/gtest.h>

#include "core/ipc/lcl_protocol.hpp"
#include "core/ipc/raster_protocol.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "render/raster_canvas.hpp"

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

namespace protocol = lcl::protocol;
namespace raster = lcl::raster_protocol;

const std::filesystem::path& runtimeDirectory() {
    static const auto directory = std::filesystem::path("/tmp") /
        ("lcl-raster-integration-" + std::to_string(getpid()));
    return directory;
}

class RasterDaemon final {
public:
    RasterDaemon() {
        std::filesystem::create_directories(runtimeDirectory());
        m_socketPath = (runtimeDirectory() / "lcl-raster.sock").string();
    }

    ~RasterDaemon() {
        if (m_privateFd >= 0) close(m_privateFd);
        if (m_pid > 0) {
            kill(m_pid, SIGTERM);
            waitpid(m_pid, nullptr, 0);
        }
        std::error_code error;
        std::filesystem::remove(m_socketPath, error);
        std::filesystem::remove(runtimeDirectory(), error);
    }

    bool start() {
        int channels[2]{-1, -1};
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channels) != 0) return false;
        m_pid = fork();
        if (m_pid == 0) {
            close(channels[0]);
            const std::string descriptor = std::to_string(channels[1]);
            execl(LCL_TEST_RASTERD_PATH, LCL_TEST_RASTERD_PATH,
                  "--compositor-fd", descriptor.c_str(),
                  "--socket", m_socketPath.c_str(), nullptr);
            std::cerr << "exec failed for " << LCL_TEST_RASTERD_PATH
                      << ": " << std::strerror(errno) << "\n";
            _exit(127);
        }
        close(channels[1]);
        m_privateFd = channels[0];

        raster::Header header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        if (!receiveWithin(m_privateFd, header, payload, receivedFd, 2000)) {
            int status = 0;
            const pid_t exited = waitpid(m_pid, &status, WNOHANG);
            std::cerr << "rasterd readiness failed; fd=" << m_privateFd
                      << " child=" << m_pid << " exited=" << exited
                      << " status=" << status << "\n";
            if (exited == m_pid) m_pid = -1;
            return false;
        }
        if (receivedFd >= 0) close(receivedFd);
        if (header.opcode != raster::Opcode::Ready || !payload.empty()) {
            std::cerr << "unexpected readiness packet opcode="
                      << static_cast<uint32_t>(header.opcode)
                      << " payload=" << payload.size() << "\n";
        }
        return header.opcode == raster::Opcode::Ready && payload.empty();
    }

    bool registerSurface(const raster::SurfaceGrant& grant) const {
        return raster::sendPacket(
            m_privateFd, raster::Opcode::RegisterSurface, grant);
    }

    bool releaseLayer(
            uint64_t layerId,
            raster::LayerReleaseReason reason =
                raster::LayerReleaseReason::Presented) const {
        raster::ReleaseLayer release{};
        release.layerId = layerId;
        release.reason = reason;
        return raster::sendPacket(
            m_privateFd, raster::Opcode::ReleaseLayer, release);
    }

    bool takeLayer(raster::LayerReady& ready, int& layerFd,
                   int timeoutMs) const {
        raster::Header header{};
        std::vector<uint8_t> payload;
        if (!receiveWithin(m_privateFd, header, payload, layerFd, timeoutMs)) {
            return false;
        }
        const auto* layer = raster::payloadAs<raster::LayerReady>(
            header, payload, raster::Opcode::LayerReady);
        if (!layer) return false;
        ready = *layer;
        return true;
    }

    const std::string& socketPath() const noexcept { return m_socketPath; }

private:
    static bool receiveWithin(int fd, raster::Header& header,
                              std::vector<uint8_t>& payload,
                              int& receivedFd, int timeoutMs) {
        pollfd descriptor{fd, POLLIN, 0};
        if (poll(&descriptor, 1, timeoutMs) <= 0) return false;
        return raster::receivePacket(fd, header, payload, receivedFd) ==
            raster::ReceiveStatus::Received;
    }

    std::string m_socketPath;
    pid_t m_pid{-1};
    int m_privateFd{-1};
};

class FakeCompositorSocket final {
public:
    FakeCompositorSocket() {
        std::filesystem::create_directories(runtimeDirectory());
        m_path = (runtimeDirectory() / "lcl-compositor.sock").string();
    }

    ~FakeCompositorSocket() {
        if (m_clientFd >= 0) close(m_clientFd);
        if (m_listenerFd >= 0) close(m_listenerFd);
        std::error_code error;
        std::filesystem::remove(m_path, error);
        std::filesystem::remove(runtimeDirectory(), error);
    }

    bool listenForClient() {
        m_listenerFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (m_listenerFd < 0) return false;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, m_path.c_str(),
                     sizeof(address.sun_path) - 1);
        return bind(m_listenerFd, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0 && listen(m_listenerFd, 1) == 0;
    }

    bool acceptClient() {
        m_clientFd = accept4(m_listenerFd, nullptr, nullptr, SOCK_CLOEXEC);
        return m_clientFd >= 0;
    }

    bool receiveSurfaceCreate(protocol::LCLMsgSurfaceCreate& create,
                              pid_t& ownerPid) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            protocol::LCLHeader header{};
            std::vector<uint8_t> payload;
            int receivedFd = -1;
            const auto status = protocol::recvPacketWithFd(
                m_clientFd, header, payload, receivedFd);
            if (receivedFd >= 0) close(receivedFd);
            if (status != protocol::ReceiveStatus::Received) return false;
            if (header.opcode != protocol::LCLOpcode::SurfaceCreate ||
                payload.size() != sizeof(create)) {
                continue;
            }
            std::memcpy(&create, payload.data(), sizeof(create));
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            if (getsockopt(m_clientFd, SOL_SOCKET, SO_PEERCRED,
                           &credentials, &length) != 0) {
                return false;
            }
            ownerPid = credentials.pid;
            return true;
        }
        return false;
    }

    bool configure(const raster::SurfaceGrant& grant,
                   float width, float height) const {
        protocol::LCLHeader grantHeader{};
        grantHeader.opcode = protocol::LCLOpcode::SurfaceProducerGrant;
        grantHeader.requestId = 1;
        grantHeader.payloadSize = sizeof(protocol::LCLMsgSurfaceProducerGrant);
        protocol::LCLMsgSurfaceProducerGrant producer{};
        producer.surfaceId = grant.surfaceId;
        producer.ownerPid = grant.ownerPid;
        producer.flags = grant.flags;
        producer.tokenHigh = grant.tokenHigh;
        producer.tokenLow = grant.tokenLow;
        if (!protocol::sendMsgWithFd(m_clientFd, grantHeader, &producer)) {
            return false;
        }

        protocol::LCLHeader configureHeader{};
        configureHeader.opcode = protocol::LCLOpcode::ConfigureBounds;
        configureHeader.requestId = 2;
        configureHeader.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);
        protocol::LCLMsgConfigureBounds configure{};
        configure.surfaceId = grant.surfaceId;
        configure.configureSerial = 1;
        configure.geometryGeneration = 1;
        configure.width = width;
        configure.height = height;
        configure.backingWidth = width;
        configure.backingHeight = height;
        configure.bufferScale = 1.0f;
        configure.resizeReason = protocol::LCLConfigureResizeReason::Initial;
        configure.isFocused = 1;
        return protocol::sendMsgWithFd(
            m_clientFd, configureHeader, &configure);
    }

    bool discard(const raster::LayerReady& ready) const {
        protocol::LCLHeader header{};
        header.opcode = protocol::LCLOpcode::FrameDiscarded;
        header.payloadSize = sizeof(protocol::LCLMsgFrameDiscarded);
        protocol::LCLMsgFrameDiscarded discarded{};
        discarded.surfaceId = ready.grant.surfaceId;
        discarded.configureSerial = ready.configureSerial;
        discarded.frameSerial = ready.frameSerial;
        discarded.geometryGeneration = ready.geometryGeneration;
        discarded.reason = protocol::LCLFrameDiscardReason::InvalidFrame;
        return protocol::sendMsgWithFd(m_clientFd, header, &discarded);
    }

    const std::string& path() const noexcept { return m_path; }

private:
    std::string m_path;
    int m_listenerFd{-1};
    int m_clientFd{-1};
};

TEST(RasterServiceIntegrationTest,
     WindowAppPublishesFirstConfiguredFrameThroughRasterDaemon) {
    RasterDaemon daemon;
    ASSERT_TRUE(daemon.start());

    FakeCompositorSocket compositor;
    ASSERT_TRUE(compositor.listenForClient());

    lcl::ui::WindowApp app(
        lcl::render::makeDisplayListCanvas(), 80.0f, 60.0f,
        "Raster integration");
    app.setAppId("org.lcl.test.raster-integration");
    int grantsReceived = 0;
    int configuresReceived = 0;
    app.setOnIpcMessage([&](const protocol::LCLHeader& header,
                            const std::vector<uint8_t>&) {
        if (header.opcode == protocol::LCLOpcode::SurfaceProducerGrant) {
            ++grantsReceived;
        } else if (header.opcode == protocol::LCLOpcode::ConfigureBounds) {
            ++configuresReceived;
        }
    });
    auto root = std::make_unique<lcl::ui::Container>();
    root->setBackgroundColor({12, 34, 56, 255});
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.connectCompositor(compositor.path()));
    ASSERT_TRUE(compositor.acceptClient());

    protocol::LCLMsgSurfaceCreate create{};
    pid_t ownerPid = 0;
    ASSERT_TRUE(compositor.receiveSurfaceCreate(create, ownerPid));
    const raster::SurfaceGrant grant{
        create.surfaceId, static_cast<int32_t>(ownerPid), 0, 101, 103};
    ASSERT_TRUE(daemon.registerSurface(grant));
    ASSERT_TRUE(compositor.configure(grant, 80.0f, 60.0f));

    raster::LayerReady ready{};
    int layerFd = -1;
    int renderedFrames = 0;
    const auto firstLayerDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (layerFd < 0 &&
           std::chrono::steady_clock::now() < firstLayerDeadline) {
        if (app.tick()) ++renderedFrames;
        if (daemon.takeLayer(ready, layerFd, 20)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(grantsReceived, 1);
    EXPECT_EQ(configuresReceived, 1);
    EXPECT_GT(renderedFrames, 0);
    ASSERT_GE(layerFd, 0);
    EXPECT_EQ(ready.grant.surfaceId, create.surfaceId);
    EXPECT_EQ(ready.grant.ownerPid, ownerPid);
    EXPECT_EQ(ready.configureSerial, 1u);
    EXPECT_EQ(ready.geometryGeneration, 1u);
    EXPECT_EQ(ready.width, 80u);
    EXPECT_EQ(ready.height, 60u);
    EXPECT_GE(ready.backingWidth, ready.width);
    EXPECT_GE(ready.backingHeight, ready.height);
    if (ready.transport == raster::LayerTransport::DmaBuf) {
        EXPECT_EQ(ready.format, lcl::platform::kDmaBufFormatArgb8888);
        EXPECT_EQ(ready.byteSize, 0u);

        // Model a guest compositor which cannot import the producer's DMA-BUF.
        // The rejection must disable that transport for only this surface and
        // reopen the app's exact frame credit so its retry arrives via SHM.
        close(layerFd);
        layerFd = -1;
        ASSERT_TRUE(daemon.releaseLayer(
            ready.layerId, raster::LayerReleaseReason::RejectedTransport));
        ASSERT_TRUE(compositor.discard(ready));

        raster::LayerReady fallback{};
        const auto fallbackDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (layerFd < 0 &&
               std::chrono::steady_clock::now() < fallbackDeadline) {
            (void)app.tick();
            if (daemon.takeLayer(fallback, layerFd, 20)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_GE(layerFd, 0);
        EXPECT_EQ(fallback.transport, raster::LayerTransport::Shm);
        EXPECT_EQ(fallback.configureSerial, ready.configureSerial);
        EXPECT_EQ(fallback.geometryGeneration, ready.geometryGeneration);
        EXPECT_EQ(fallback.byteSize,
                  static_cast<uint64_t>(fallback.stride) *
                      fallback.backingHeight);
    } else {
        EXPECT_EQ(ready.transport, raster::LayerTransport::Shm);
        EXPECT_EQ(ready.byteSize,
                  static_cast<uint64_t>(ready.stride) * ready.backingHeight);
    }
    close(layerFd);
}

} // namespace
