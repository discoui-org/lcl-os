#include <gtest/gtest.h>

#include "system/ipc/lcl_protocol.hpp"
#include "system/ipc/raster_protocol.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/navigation_split_view.hpp"
#include "lcl-ui/widgets/progress_view.hpp"
#include "system/render/raster_canvas.hpp"
#include "system/render/client_egl_context.hpp"
#include "lcl-client/surface_client.hpp"
#include "apps/sandbox_probe/producer_grant_probe.hpp"

#include <GLES2/gl2.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
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

    bool revokeSurface(const raster::SurfaceGrant& grant) const {
        return raster::sendPacket(m_privateFd, raster::Opcode::RevokeSurface, grant);
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

    bool takeLayer(raster::LayerReady& ready, int& layerFd, int timeoutMs,
                   raster::PresentationFrameReady* presentation = nullptr,
                   bool* animationCompleted = nullptr) const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            raster::Header header{};
            std::vector<uint8_t> payload;
            layerFd = -1;
            if (!receiveWithin(
                    m_privateFd, header, payload, layerFd,
                    attempt == 0 ? timeoutMs : 0)) {
                return false;
            }
            const auto* layer = raster::payloadAs<raster::LayerReady>(
                header, payload, raster::Opcode::LayerReady);
            if (layer) {
                ready = *layer;
                m_lastStorageFrameSerial = ready.frameSerial;
                return true;
            }
            raster::PresentationFrameReady frame{};
            std::vector<raster::PresentationLayerState> layers;
            if (raster::decodePresentationFrameReady(header, payload, frame,
                                                     layers)) {
                // A normal raster output is followed by its snapshot
                // manifest. The fake compositor has already presented that
                // root storage; skip only this matching declaration. A later
                // manifest without fresh storage is the retained-property
                // path and must receive FramePresented as well.
                if (frame.frameSerial != m_lastStorageFrameSerial &&
                    presentation) {
                    ready = {};
                    *presentation = frame;
                    if (layerFd >= 0) close(layerFd);
                    layerFd = -1;
                    return true;
                }
            }
            raster::PresentationAnimation animation{};
            if (animationCompleted && raster::decodePresentationAnimation(
                    header, payload, animation)) {
                raster::PresentationAnimationResult result{};
                result.grant = animation.grant;
                result.transactionId = animation.transactionId;
                result.nodeId = animation.nodeId;
                result.outcome =
                    raster::PresentationAnimationOutcome::Completed;
                if (layerFd >= 0) close(layerFd);
                layerFd = -1;
                if (!raster::sendPacket(
                        m_privateFd, raster::Opcode::PresentationAnimationResult,
                        result)) {
                    return false;
                }
                *animationCompleted = true;
                // The client normally submitted the matching retained-state
                // manifest in the same tick. Keep draining so this fake
                // compositor presents it before the next input sample.
                continue;
            }
            if (layerFd >= 0) close(layerFd);
            layerFd = -1;
        }
        return false;
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
    mutable uint64_t m_lastStorageFrameSerial{0};
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

    bool present(const raster::LayerReady& ready) const {
        return present(ready.grant, ready.configureSerial, ready.frameSerial,
                       ready.geometryGeneration);
    }

    bool present(const raster::PresentationFrameReady& frame) const {
        return present(frame.grant, frame.configureSerial, frame.frameSerial,
                       frame.geometryGeneration);
    }

private:
    bool present(const raster::SurfaceGrant& grant, uint64_t configureSerial,
                 uint64_t frameSerial, uint64_t geometryGeneration) const {
        protocol::LCLHeader header{};
        header.opcode = protocol::LCLOpcode::FramePresented;
        header.payloadSize = sizeof(protocol::LCLMsgFramePresented);
        protocol::LCLMsgFramePresented presented{};
        presented.surfaceId = grant.surfaceId;
        presented.configureSerial = configureSerial;
        presented.frameSerial = frameSerial;
        presented.geometryGeneration = geometryGeneration;
        presented.displaySequence = frameSerial;
        presented.timestampNs = 1;
        presented.refreshIntervalNs = 16666667;
        return protocol::sendMsgWithFd(m_clientFd, header, &presented);
    }

public:

    const std::string& path() const noexcept { return m_path; }

private:
    std::string m_path;
    int m_listenerFd{-1};
    int m_clientFd{-1};
};

TEST(RasterServiceIntegrationTest, ProducerGrantRejectsOtherProcessesAndTransferredSockets) {
    using namespace lcl::sandbox_probe;
    RasterDaemon daemon;
    ASSERT_TRUE(daemon.start());
    const raster::SurfaceGrant grant{7, static_cast<int32_t>(getpid()), 0, 101, 103};
    ASSERT_TRUE(daemon.registerSurface(grant));
    ProbeFd owner(connectRaster(daemon.socketPath()));
    ASSERT_GE(owner.get(), 0);
    ASSERT_TRUE(checkGrant(owner.get(), grant, raster::DiscardReason::InvalidFrame));
    EXPECT_TRUE(rejectsChildGrant(daemon.socketPath(), grant, GrantAttack::NewConnection));
    EXPECT_TRUE(rejectsChildGrant(daemon.socketPath(), grant, GrantAttack::RewrittenOwner));
    EXPECT_TRUE(rejectsChildGrant(daemon.socketPath(), grant, GrantAttack::InheritedConnection));
    // An attacker cannot break the owner's independent connection or grant.
    EXPECT_TRUE(checkGrant(owner.get(), grant, raster::DiscardReason::InvalidFrame));
    raster::LayerReady ready{};
    int layerFd = -1;
    EXPECT_FALSE(daemon.takeLayer(ready, layerFd, 50));
    if (layerFd >= 0) close(layerFd);
}

TEST(RasterServiceIntegrationTest, ProducerGrantRejectsTamperingAndRevocation) {
    using namespace lcl::sandbox_probe;
    RasterDaemon daemon;
    ASSERT_TRUE(daemon.start());
    const raster::SurfaceGrant grant{7, static_cast<int32_t>(getpid()), 0, 107, 109};
    ASSERT_TRUE(daemon.registerSurface(grant));
    ProbeFd owner(connectRaster(daemon.socketPath()));
    ASSERT_GE(owner.get(), 0);
    ASSERT_TRUE(checkGrant(owner.get(), grant, raster::DiscardReason::InvalidFrame));
    for (int field = 0; field < 5; ++field) {
        auto altered = grant;
        if (field == 0) ++altered.surfaceId;
        if (field == 1) ++altered.ownerPid;
        if (field == 2) altered.flags = raster::kGrantInteractiveSystem;
        if (field == 3) ++altered.tokenHigh;
        if (field == 4) ++altered.tokenLow;
        EXPECT_TRUE(checkGrant(owner.get(), altered, raster::DiscardReason::InvalidGrant));
    }
    ASSERT_TRUE(daemon.revokeSurface(grant));
    // Private revoke and public producer packets use different channels; allow
    // an already-running client drain to finish before asserting revocation.
    bool revoked = false;
    for (int attempt = 0; attempt < 20 && !revoked; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        revoked = checkGrant(owner.get(), grant, raster::DiscardReason::InvalidGrant);
    }
    EXPECT_TRUE(revoked);
}

TEST(RasterServiceIntegrationTest, NativeUploadRejectsNonDmaBufDescriptor) {
    using namespace lcl::sandbox_probe;
    RasterDaemon daemon;
    ASSERT_TRUE(daemon.start());
    const raster::SurfaceGrant grant{
        12, static_cast<int32_t>(getpid()), 0, 151, 157};
    ASSERT_TRUE(daemon.registerSurface(grant));
    ProbeFd owner(connectRaster(daemon.socketPath()));
    ASSERT_GE(owner.get(), 0);
    const int notDmaBuf = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(notDmaBuf, 0);
    raster::UploadExternalBuffer upload{};
    upload.grant = grant;
    upload.bufferId = 9;
    upload.contentRevision = 1;
    upload.transport = raster::ExternalBufferTransport::DmaBufArgb8888;
    upload.width = 64;
    upload.height = 48;
    upload.stride = 64 * sizeof(uint32_t);
    upload.format = lcl::platform::kDmaBufFormatArgb8888;
    ASSERT_TRUE(raster::sendPacket(
        owner.get(), raster::Opcode::UploadExternalBuffer, upload, notDmaBuf));
    close(notDmaBuf);
    pollfd descriptor{owner.get(), POLLIN, 0};
    ASSERT_GT(poll(&descriptor, 1, 1000), 0);
    raster::Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(raster::receivePacket(
                  owner.get(), header, payload, receivedFd),
              raster::ReceiveStatus::Received);
    const auto* released = raster::payloadAs<raster::ExternalBufferReleased>(
        header, payload, raster::Opcode::ExternalBufferReleased);
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->reason,
              raster::ExternalBufferReleaseReason::Rejected);
    EXPECT_EQ(receivedFd, -1);
}

TEST(RasterServiceIntegrationTest,
     SurfaceClientPublishesDmaBufWithoutDisplayListAndReceivesRelease) {
    RasterDaemon daemon;
    ASSERT_TRUE(daemon.start());
    FakeCompositorSocket compositor;
    ASSERT_TRUE(compositor.listenForClient());

    lcl::client::SurfaceOptions options{};
    options.surfaceId = 17;
    options.appId = "org.lcl.test.native-client";
    options.title = "Native client integration";
    options.bounds = {0.0f, 0.0f, 64.0f, 48.0f};
    lcl::client::SurfaceClient surface;
    ASSERT_TRUE(surface.connect(options, compositor.path(), daemon.socketPath()));
    ASSERT_TRUE(compositor.acceptClient());
    protocol::LCLMsgSurfaceCreate create{};
    pid_t ownerPid = 0;
    ASSERT_TRUE(compositor.receiveSurfaceCreate(create, ownerPid));
    const raster::SurfaceGrant grant{
        create.surfaceId, static_cast<int32_t>(ownerPid), 0, 201, 203};
    ASSERT_TRUE(daemon.registerSurface(grant));
    ASSERT_TRUE(compositor.configure(grant, 64.0f, 48.0f));

    const auto connectDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while ((!surface.hasConfigure() || surface.rasterFd() < 0) &&
           std::chrono::steady_clock::now() < connectDeadline) {
        (void)surface.dispatch();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(surface.hasConfigure());
    ASSERT_GE(surface.rasterFd(), 0);

    lcl::render::ClientEGLContext context;
    if (!context.initialize(64, 48) || !context.hasDmaBufPool()) {
        GTEST_SKIP() << "No EGL DMA-BUF export device in this test environment";
    }
    const auto target = context.acquireDmaBufTarget();
    ASSERT_TRUE(target.has_value());
    glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
    glViewport(0, 0, 64, 48);
    glClearColor(0.2f, 0.5f, 0.9f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto exported = context.exportCurrentDmaBuf();
    ASSERT_TRUE(exported.has_value());
    ASSERT_FALSE(exported->androidHardwareBuffer);
    ASSERT_GE(exported->fd, 0);

    lcl::client::DmaBufFrame frame{};
    frame.bufferId = exported->bufferId;
    frame.contentRevision = 1;
    frame.width = exported->width;
    frame.height = exported->height;
    frame.stride = exported->stride;
    frame.format = exported->format;
    frame.modifier = exported->modifier;
    frame.damage = {0.0f, 0.0f, 64.0f, 48.0f};
    frame.opaque = true;
    frame.buffer = lcl::client::OwnedFd(exported->fd);
    frame.acquireFence = lcl::client::OwnedFd(exported->acquireFenceFd);
    ASSERT_TRUE(surface.submitFrame(std::move(frame)));

    raster::LayerReady ready{};
    int layerFd = -1;
    ASSERT_TRUE(daemon.takeLayer(ready, layerFd, 3000));
    ASSERT_GE(layerFd, 0);
    EXPECT_EQ(ready.transport, raster::LayerTransport::DmaBuf);
    EXPECT_EQ(ready.frameSerial, 1u);
    EXPECT_EQ(ready.configureSerial, 1u);
    EXPECT_EQ(ready.width, 64u);
    EXPECT_EQ(ready.height, 48u);
    close(layerFd);
    ASSERT_TRUE(daemon.releaseLayer(ready.layerId));
    ASSERT_TRUE(compositor.present(ready));

    bool presented = false;
    bool released = false;
    const auto releaseDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!(presented && released) &&
           std::chrono::steady_clock::now() < releaseDeadline) {
        for (auto& event : surface.dispatch()) {
            if (std::get_if<lcl::client::FramePresentedEvent>(&event)) {
                presented = true;
            } else if (auto* buffer =
                           std::get_if<lcl::client::BufferReleasedEvent>(&event)) {
                EXPECT_EQ(buffer->bufferId, exported->bufferId);
                context.releaseDmaBuf(
                    static_cast<uint32_t>(buffer->bufferId),
                    buffer->releaseFence.release());
                released = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(presented);
    EXPECT_TRUE(released);
}

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
    auto patch = std::make_unique<lcl::ui::Container>();
    auto* patchPtr = patch.get();
    patch->setWidth(10.0f);
    patch->setHeight(10.0f);
    patch->setBackgroundColor({80, 90, 100, 255});
    root->addChild(std::move(patch));
    auto spinner = std::make_unique<lcl::ui::ProgressView>();
    auto* spinnerPtr = spinner.get();
    root->addChild(std::move(spinner));
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
    EXPECT_EQ(ready.damageX, 0u);
    EXPECT_EQ(ready.damageY, 0u);
    EXPECT_EQ(ready.damageWidth, ready.width);
    EXPECT_EQ(ready.damageHeight, ready.height);
    EXPECT_GE(ready.backingWidth, ready.width);
    EXPECT_GE(ready.backingHeight, ready.height);
    if (ready.transport == raster::LayerTransport::DmaBuf) {
        EXPECT_NE(ready.bufferId, 0u);
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
        EXPECT_EQ(fallback.bufferId, 0u);
        EXPECT_EQ(fallback.configureSerial, ready.configureSerial);
        EXPECT_EQ(fallback.geometryGeneration, ready.geometryGeneration);
        EXPECT_EQ(fallback.byteSize,
                  static_cast<uint64_t>(fallback.stride) *
                      fallback.backingHeight);
        ready = fallback;
    } else {
        EXPECT_EQ(ready.transport, raster::LayerTransport::Shm);
        EXPECT_EQ(ready.bufferId, 0u);
        EXPECT_EQ(ready.byteSize,
                  static_cast<uint64_t>(ready.stride) * ready.backingHeight);
    }
    const uint64_t blockedPresentationRevision =
        spinnerPtr->getPresentationRevision();
    for (int attempt = 0; attempt < 8; ++attempt) {
        (void)app.tick();
    }
    EXPECT_EQ(spinnerPtr->getPresentationRevision(),
              blockedPresentationRevision);
    close(layerFd);
    layerFd = -1;
    ASSERT_TRUE(daemon.releaseLayer(ready.layerId));
    ASSERT_TRUE(compositor.present(ready));

    patchPtr->setBackgroundColor({180, 40, 70, 255});
    raster::LayerReady patchReady{};
    const auto patchDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (layerFd < 0 && std::chrono::steady_clock::now() < patchDeadline) {
        (void)app.tick();
        if (daemon.takeLayer(patchReady, layerFd, 20)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GE(layerFd, 0);
    EXPECT_GT(spinnerPtr->getPresentationRevision(),
              blockedPresentationRevision);
    EXPECT_GT(patchReady.frameSerial, ready.frameSerial);
    EXPECT_LT(patchReady.damageWidth, patchReady.width);
    EXPECT_LT(patchReady.damageHeight, patchReady.height);
    close(layerFd);
    ASSERT_TRUE(daemon.releaseLayer(patchReady.layerId));
}

TEST(RasterServiceIntegrationTest,
     NavigationEntryAndTouchBackReuseClippedPagePixels) {
    using namespace lcl::ui;
    class CountingPage final : public Container {
    public:
        int paints{0};
        void draw(lcl::graphics::Canvas& canvas,
                  const lcl::graphics::RectF& damage) override {
            ++paints;
            Container::draw(canvas, damage);
        }
    };
    RasterDaemon daemon;
    ASSERT_TRUE(daemon.start());
    FakeCompositorSocket compositor;
    ASSERT_TRUE(compositor.listenForClient());
    WindowApp app(lcl::render::makeDisplayListCanvas(), 360, 640,
                  "Navigation retained pixels");
    app.setAppId("org.lcl.test.navigation-retained");
    auto navigation = std::make_unique<NavigationSplitView>();
    auto* split = navigation.get();
    auto page = std::make_unique<CountingPage>();
    auto* pagePtr = page.get();
    page->setBackgroundColor({20, 50, 90, 255});
    // The real navigation viewport is a clipped descendant of the moving
    // NavigationStack. Add another clip to exercise inherited bounds too.
    page->setClipsToBounds(true);
    ASSERT_TRUE(split->detailNavigation().setRootPage({
        .route = NavigationRoute("general"),
        .title = "General",
        .content = std::move(page),
    }));
    split->setSidebar(std::make_unique<Container>());
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.connectCompositor(compositor.path()));
    ASSERT_TRUE(compositor.acceptClient());
    protocol::LCLMsgSurfaceCreate create{};
    pid_t ownerPid = 0;
    ASSERT_TRUE(compositor.receiveSurfaceCreate(create, ownerPid));
    const raster::SurfaceGrant grant{
        create.surfaceId, static_cast<int32_t>(ownerPid), 0, 201, 203};
    ASSERT_TRUE(daemon.registerSurface(grant));
    ASSERT_TRUE(compositor.configure(grant, 360, 640));

    const auto publish = [&](bool allowIdle = false) -> bool {
        raster::LayerReady ready{};
        raster::PresentationFrameReady presentation{};
        bool animationCompleted = false;
        int layerFd = -1;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            (void)app.tick();
            if (!daemon.takeLayer(
                    ready, layerFd, 10, &presentation, &animationCompleted)) {
                // A compositor-owned spring advances immutable presentation
                // layers without another client/raster transaction. Once its
                // completion result has been consumed, this is a successful
                // idle step rather than a missing frame.
                if (allowIdle && !app.hasActiveAnimations()) return true;
                continue;
            }
            if (presentation.frameSerial != 0) {
                return compositor.present(presentation);
            }
            if (layerFd >= 0) close(layerFd);
            return daemon.releaseLayer(ready.layerId) && compositor.present(ready);
        }
        return false;
    };
    ASSERT_TRUE(publish());
    split->presentDetail();
    // Establish the new page's complete composition template.
    for (int frame = 0; frame < 3; ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
        ASSERT_TRUE(publish());
    }
    const int entryPaints = pagePtr->paints;
    ASSERT_GT(entryPaints, 0);
    for (int frame = 0; frame < 5; ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
        ASSERT_TRUE(publish(true));
    }
    EXPECT_EQ(pagePtr->paints, entryPaints);

    for (int frame = 0; frame < 240 && split->isTransitioning(); ++frame) {
        app.advanceAnimations(1.0f / 60.0f);
    }
    // Compositor-owned springs report completion over rasterd's private
    // channel, so consume that result before checking transition ownership.
    ASSERT_TRUE(publish(true));
    ASSERT_FALSE(split->isTransitioning());
    ASSERT_TRUE(publish(true));
    app.sendPointerDown(4, 100, 0, PointerSource::Touch, 51);
    for (int x : {24, 44, 64}) {
        app.sendPointerMove(x, 100, PointerSource::Touch, 51);
        ASSERT_TRUE(publish());
    }
    const int dragPaints = pagePtr->paints;
    for (int x : {84, 104, 124}) {
        app.sendPointerMove(x, 100, PointerSource::Touch, 51);
        ASSERT_TRUE(publish());
    }
    EXPECT_EQ(pagePtr->paints, dragPaints);

    // A real content change inside the cached page must still be painted.
    pagePtr->setBackgroundColor({90, 50, 20, 255});
    ASSERT_TRUE(publish());
    EXPECT_GT(pagePtr->paints, dragPaints);
    const int contentPaints = pagePtr->paints;
    pagePtr->setOpacity(0.8f);
    ASSERT_TRUE(publish());
    EXPECT_GT(pagePtr->paints, contentPaints);
    app.sendPointerUp(124, 100, 0, PointerSource::Touch, 51);
    EXPECT_FALSE(split->isDetailPresented());
}

} // namespace
