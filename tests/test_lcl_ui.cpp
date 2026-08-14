#include <gtest/gtest.h>
#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/layout/yoga_node.hpp"
#include "lcl-ui/widgets/widget.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/window_chrome.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "render/skia_renderer.hpp"
#include "render/skia_canvas.hpp"
#include "render/backdrop_filter_geometry.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

using namespace lcl::ui;

namespace {

class RecordingCanvas final : public Canvas {
public:
    bool initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) override {
        initialized = true;
        setTargetPixels(targetPixels, width, height);
        return true;
    }

    void setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) override {
        ++targetSetCount;
        pixels = targetPixels;
        pixelWidth = width;
        pixelHeight = height;
    }

    void setContentScale(float value) override { contentScale = value; }
    void beginFrame() override { ++beginCount; }
    void endFrame() override { ++endCount; }
    uint32_t* rasterBuffer() override { return pixels; }
    void setDmaBufTransportEnabled(bool enabled) override { dmaBufEnabled = enabled; }
    bool hasDmaBufTransport() const override { return dmaBufAvailable && dmaBufEnabled; }
    bool configureDmaBufFrame(uint32_t contentWidth, uint32_t contentHeight,
                              uint32_t backingWidth, uint32_t backingHeight) override {
        if (!hasDmaBufTransport()) return false;
        if (backingWidth > dmaCapacityWidth || backingHeight > dmaCapacityHeight) {
            dmaCapacityWidth = std::max(dmaCapacityWidth, backingWidth);
            dmaCapacityHeight = std::max(dmaCapacityHeight, backingHeight);
            ++dmaCapacityGrowCount;
        }
        dmaContentWidth = contentWidth;
        dmaContentHeight = contentHeight;
        dmaBackingWidth = backingWidth;
        dmaBackingHeight = backingHeight;
        ++dmaConfigureCount;
        return true;
    }
    bool isDmaBufFrameActive() const override { return hasDmaBufTransport(); }
    std::optional<DmaBufFrame> takeDmaBufFrame() override {
        if (!hasDmaBufTransport()) return std::nullopt;
        const int fd = dup(STDIN_FILENO);
        if (fd < 0) return std::nullopt;
        return DmaBufFrame{nextBufferId++, dmaContentWidth, dmaContentHeight,
                           dmaBackingWidth, dmaBackingHeight, dmaBackingWidth * 4,
                           lcl::protocol::LCL_BUFFER_FORMAT_ARGB8888, ~uint64_t{0}, fd};
    }
    void releaseDmaBufFrame(uint32_t bufferId) override { releasedBufferIds.push_back(bufferId); }

    void drawRect(const Rect& rect, Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
    }

    void drawRoundedRect(const Rect& rect, float radius, Color color,
                         Color border, float borderWidth, float roundness) override {
        roundedRects.push_back(rect);
        roundedRadii.push_back(radius);
        colors.push_back(color);
        borders.push_back(border);
        borderWidths.push_back(borderWidth);
        roundnesses.push_back(roundness);
    }

    void drawTopRoundedRect(const Rect& rect, float radius, Color color,
                            float roundness) override {
        topRoundedRects.push_back(rect);
        roundedRadii.push_back(radius);
        colors.push_back(color);
        roundnesses.push_back(roundness);
    }

    void drawText(float x, float y, const std::string& text, Color color,
                  float fontSize, FontFamily family) override {
        textPositions.push_back({x, y, 0.0f, 0.0f});
        texts.push_back(text);
        colors.push_back(color);
        fontSizes.push_back(fontSize);
        fontFamilies.push_back(family);
    }

    void drawRasterizedText(float x, float y, const std::string& text, Color color,
                            float fontSize, FontFamily family) override {
        rasterTextPositions.push_back({x, y, 0.0f, 0.0f});
        rasterTexts.push_back(text);
        colors.push_back(color);
        fontSizes.push_back(fontSize);
        fontFamilies.push_back(family);
    }

    float measureText(const std::string& text, float fontSize, FontFamily) override {
        return static_cast<float>(text.size()) * fontSize * 0.6f;
    }

    void drawBuffer(int, int, int, int, const uint32_t*, int, float,
                    float, float, bool, int, int) override {
        ++bufferDrawCount;
    }

    bool initialized{false};
    uint32_t* pixels{nullptr};
    uint32_t pixelWidth{0};
    uint32_t pixelHeight{0};
    float contentScale{1.0f};
    int beginCount{0};
    int endCount{0};
    int bufferDrawCount{0};
    int targetSetCount{0};
    bool dmaBufAvailable{false};
    bool dmaBufEnabled{false};
    uint32_t dmaContentWidth{0};
    uint32_t dmaContentHeight{0};
    uint32_t dmaBackingWidth{0};
    uint32_t dmaBackingHeight{0};
    uint32_t dmaCapacityWidth{0};
    uint32_t dmaCapacityHeight{0};
    uint32_t nextBufferId{1};
    int dmaConfigureCount{0};
    int dmaCapacityGrowCount{0};
    std::vector<uint32_t> releasedBufferIds;
    std::vector<Rect> rects;
    std::vector<Rect> roundedRects;
    std::vector<Rect> topRoundedRects;
    std::vector<Rect> textPositions;
    std::vector<Rect> rasterTextPositions;
    std::vector<float> roundedRadii;
    std::vector<float> borderWidths;
    std::vector<float> roundnesses;
    std::vector<float> fontSizes;
    std::vector<FontFamily> fontFamilies;
    std::vector<Color> colors;
    std::vector<Color> borders;
    std::vector<std::string> texts;
    std::vector<std::string> rasterTexts;
};

} // namespace

TEST(LclUiTest, RectMath) {
    Rect r1{10.0f, 10.0f, 50.0f, 50.0f};
    Rect r2{30.0f, 30.0f, 50.0f, 50.0f};

    EXPECT_TRUE(r1.intersects(r2));
    EXPECT_TRUE(r1.containsPoint(20.0f, 20.0f));
    EXPECT_FALSE(r1.containsPoint(70.0f, 70.0f));

    Rect intersection = r1.intersection(r2);
    EXPECT_EQ(intersection.x, 30.0f);
    EXPECT_EQ(intersection.y, 30.0f);
    EXPECT_EQ(intersection.width, 30.0f);
    EXPECT_EQ(intersection.height, 30.0f);

    Rect unionRect = r1.unionWith(r2);
    EXPECT_EQ(unionRect.x, 10.0f);
    EXPECT_EQ(unionRect.y, 10.0f);
    EXPECT_EQ(unionRect.width, 70.0f);
    EXPECT_EQ(unionRect.height, 70.0f);
}
TEST(LclUiTest, WidgetsUseBackendNeutralCanvas) {
    RecordingCanvas canvas;
    auto root = std::make_unique<Container>();
    root->setBackgroundColor({10, 20, 30, 255});
    root->getYogaNode().setWidth(120.0f);
    root->getYogaNode().setHeight(80.0f);

    auto label = std::make_unique<Text>("Canvas");
    label->setTextColor({230, 231, 232, 255});
    root->addChild(std::move(label));

    root->getYogaNode().calculateLayout(120.0f, 80.0f);
    root->syncLayout();
    root->draw(canvas, {0.0f, 0.0f, 120.0f, 80.0f});

    ASSERT_EQ(canvas.rects.size(), 1u);
    EXPECT_EQ(canvas.colors.front().r, 10);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.front(), "Canvas");
}

TEST(LclUiTest, WindowAppAcceptsInjectedCanvas) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 64, 48, "Injected Canvas Test");
    ASSERT_TRUE(recorded->initialized);

    auto root = std::make_unique<Container>();
    root->setBackgroundColor({1, 2, 3, 255});
    root->getYogaNode().setWidth(64.0f);
    root->getYogaNode().setHeight(48.0f);
    app.setRootWidget(std::move(root));

    EXPECT_TRUE(app.renderFrame());
    EXPECT_EQ(recorded->beginCount, 1);
    EXPECT_EQ(recorded->endCount, 1);
    ASSERT_EQ(recorded->rects.size(), 1u);
    EXPECT_EQ(recorded->rects.front().width, 64.0f);
    EXPECT_EQ(recorded->rects.front().height, 48.0f);
}

TEST(LclUiTest, WindowAppWaitsForInitialConfigureBeforeAttachingBuffer) {
    const std::string socketPath = "/tmp/lcl-ui-initial-configure-" +
        std::to_string(getpid()) + ".sock";
    unlink(socketPath.c_str());

    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Initial configure gate");
    app.setAppId("org.lcl.test.initial-configure");
    ASSERT_TRUE(app.connectCompositor(socketPath));

    const int peer = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    ASSERT_GE(peer, 0);
    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(peer, header, payload, receivedFd));
    EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::SurfaceCreate);
    if (receivedFd >= 0) close(receivedFd);

    char byte = 0;
    errno = 0;
    EXPECT_EQ(recv(peer, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    lcl::protocol::LCLMsgConfigureBounds configure{};
    configure.surfaceId = 1;
    configure.configureSerial = 7;
    // This intentionally differs by less than the resize-throttle large-jump
    // threshold. The initial configure must still be applied immediately.
    configure.width = 80;
    configure.height = 64;
    configure.backingWidth = 80;
    configure.backingHeight = 64;
    configure.bufferScale = 1.0f;
    configure.resizeReason = lcl::protocol::LCLConfigureResizeReason::Initial;
    lcl::protocol::LCLHeader configureHeader{};
    configureHeader.opcode = lcl::protocol::LCLOpcode::ConfigureBounds;
    configureHeader.payloadSize = sizeof(configure);
    ASSERT_TRUE(lcl::protocol::sendMsgWithFd(peer, configureHeader, &configure));

    ASSERT_TRUE(app.tick());
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(peer, header, payload, receivedFd));
    EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::AttachBuffer);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgAttachBuffer));
    const auto* attach = reinterpret_cast<const lcl::protocol::LCLMsgAttachBuffer*>(payload.data());
    EXPECT_EQ(attach->configureSerial, configure.configureSerial);
    EXPECT_EQ(attach->width, configure.width);
    EXPECT_EQ(attach->height, configure.height);
    EXPECT_GE(receivedFd, 0);
    if (receivedFd >= 0) close(receivedFd);

    close(peer);
    close(listener);
    unlink(socketPath.c_str());
}

TEST(LclUiTest, LiveGpuResizeCoalescesSerialsAvoidsShmAndWaitsForPresentation) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    recorded->dmaBufAvailable = true;
    WindowApp app(std::move(canvas), 64, 48, "Live GPU resize");
    app.setResizePresentationMode(lcl::protocol::LCLResizePresentationMode::Live);
    app.setExternalIpcSocket(sockets[0]);

    const auto sendConfigure = [&](uint64_t serial, uint32_t width, uint32_t height,
                                   lcl::protocol::LCLConfigureResizeReason reason) {
        lcl::protocol::LCLMsgConfigureBounds configure{};
        configure.surfaceId = app.getSurfaceId();
        configure.configureSerial = serial;
        configure.width = width;
        configure.height = height;
        configure.backingWidth = 1000;
        configure.backingHeight = 700;
        configure.bufferScale = 1.0f;
        configure.resizeReason = reason;
        lcl::protocol::LCLHeader header{};
        header.opcode = lcl::protocol::LCLOpcode::ConfigureBounds;
        header.payloadSize = sizeof(configure);
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[1], header, &configure));
    };

    sendConfigure(10, 80, 60,
                  lcl::protocol::LCLConfigureResizeReason::WindowStateTransition);
    sendConfigure(11, 96, 72,
                  lcl::protocol::LCLConfigureResizeReason::WindowStateTransition);
    ASSERT_TRUE(app.tick());
    EXPECT_EQ(app.getWidth(), 96u);
    EXPECT_EQ(app.getHeight(), 72u);
    EXPECT_EQ(recorded->targetSetCount, 1); // constructor only: no resize SHM target
    EXPECT_EQ(recorded->dmaBackingWidth, 1000u);
    EXPECT_EQ(recorded->dmaBackingHeight, 700u);
    EXPECT_EQ(recorded->dmaCapacityGrowCount, 1);

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, lcl::protocol::LCLOpcode::AttachDmaBuf);
    const auto* first = reinterpret_cast<const lcl::protocol::LCLMsgAttachDmaBuf*>(payload.data());
    EXPECT_EQ(first->configureSerial, 11u);
    EXPECT_EQ(first->width, 96u);
    EXPECT_EQ(first->backingWidth, 1000u);
    if (receivedFd >= 0) close(receivedFd);

    sendConfigure(12, 112, 84,
                  lcl::protocol::LCLConfigureResizeReason::WindowStateTransition);
    EXPECT_FALSE(app.tick());
    EXPECT_EQ(app.getWidth(), 96u);

    lcl::protocol::LCLMsgFramePresented presented{};
    presented.surfaceId = app.getSurfaceId();
    presented.timestampNs = 1000000000ull;
    presented.refreshIntervalNs = 6944444ull;
    header = {};
    header.opcode = lcl::protocol::LCLOpcode::FramePresented;
    header.payloadSize = sizeof(presented);
    ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[1], header, &presented));
    ASSERT_TRUE(app.tick());
    EXPECT_EQ(app.getWidth(), 112u);
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    const auto* second = reinterpret_cast<const lcl::protocol::LCLMsgAttachDmaBuf*>(payload.data());
    EXPECT_EQ(second->configureSerial, 12u);
    const uint32_t secondBufferId = second->bufferId;
    EXPECT_EQ(recorded->dmaCapacityGrowCount, 1);
    if (receivedFd >= 0) close(receivedFd);

    // A genuinely rejected in-flight Live frame must return the frame credit;
    // otherwise a newer configure would leave WindowApp gated forever.
    sendConfigure(13, 128, 96,
                  lcl::protocol::LCLConfigureResizeReason::WindowStateTransition);
    lcl::protocol::LCLMsgReleaseDmaBuf release{};
    release.surfaceId = app.getSurfaceId();
    release.bufferId = secondBufferId;
    header = {};
    header.opcode = lcl::protocol::LCLOpcode::ReleaseDmaBuf;
    header.payloadSize = sizeof(release);
    ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[1], header, &release));

    ASSERT_TRUE(app.tick());
    EXPECT_EQ(app.getWidth(), 128u);
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    const auto* recovered =
        reinterpret_cast<const lcl::protocol::LCLMsgAttachDmaBuf*>(payload.data());
    EXPECT_EQ(recovered->configureSerial, 13u);
    if (receivedFd >= 0) close(receivedFd);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, ResizeFirstFrameDamagesPostLayoutRootExtent) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    recorded->dmaBufAvailable = true;
    WindowApp app(std::move(canvas), 100, 100, "Resize damage");
    app.setExternalIpcSocket(sockets[0]);
    ASSERT_TRUE(recorded->configureDmaBufFrame(100, 100, 100, 100));

    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(100.0f);
    root->getYogaNode().setHeight(100.0f);
    root->getYogaNode().setAlignItems(YGAlignCenter);
    root->getYogaNode().setJustifyContent(YGJustifyCenter);
    auto child = std::make_unique<Button>("Moved");
    child->setWidth(40.0f);
    child->setHeight(24.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());

    recorded->roundedRects.clear();
    recorded->texts.clear();
    recorded->textPositions.clear();
    app.resize(400, 100);
    ASSERT_TRUE(app.renderFrame());

    ASSERT_FALSE(recorded->roundedRects.empty());
    EXPECT_GT(recorded->roundedRects.back().x, 100.0f);
    ASSERT_EQ(recorded->texts.size(), 1u);
    EXPECT_EQ(recorded->texts.front(), "Moved");

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, ImplicitTransactionInterpolatesTransformOpacityAndReflowLayout) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 320, 200, "Motion transaction");
    auto root = std::make_unique<Container>();
    root->setWidth(320.0f);
    root->setHeight(200.0f);
    auto child = std::make_unique<Button>("Animated");
    Button* pointer = child.get();
    child->setWidth(100.0f);
    child->setHeight(40.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.animate(Motion::tween(1.0f, Easing::linear()), {LayoutMode::Reflow}, [&] {
        pointer->setWidth(200.0f);
        pointer->setOpacity(0.0f);
        pointer->setTranslationX(20.0f);
    });
    EXPECT_TRUE(app.advanceAnimations(0.5f));
    EXPECT_NEAR(pointer->getAbsoluteBounds().width, 150.0f, 0.01f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.5f, 0.01f);
    EXPECT_NEAR(pointer->getPresentationState().translationX, 10.0f, 0.01f);
    EXPECT_EQ(app.getDispatcher().hitTest(app.getRootWidget(), 140.0f, 20.0f), pointer);
}

TEST(LclUiTest, MorphUsesFinalLayoutAndFreezesWindowInputUntilSettled) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 320, 200, "Morph transaction");
    auto root = std::make_unique<Container>();
    root->setWidth(320.0f);
    root->setHeight(200.0f);
    auto child = std::make_unique<Button>("Morph");
    Button* pointer = child.get();
    child->setWidth(100.0f);
    child->setHeight(40.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.animate(Motion::spring(0.32f, 0.06f), {LayoutMode::Morph}, [&] {
        pointer->setWidth(240.0f);
    });
    EXPECT_NEAR(pointer->getAbsoluteBounds().width, 240.0f, 0.01f);
    EXPECT_FALSE(app.sendPointerDown(20.0f, 20.0f));
    for (int index = 0; index < 240 && app.hasActiveAnimations(); ++index)
        app.advanceAnimations(1.0f / 240.0f);
    EXPECT_FALSE(app.hasActiveAnimations());
    EXPECT_TRUE(app.sendPointerDown(20.0f, 20.0f));
}

TEST(LclUiTest, MorphCrossfadesFrozenOldRasterIntoFinalUi) {
    WindowApp app(lcl::render::makeSkiaCanvas(), 4, 4, "Morph raster crossfade");
    auto root = std::make_unique<Container>();
    Container* pointer = root.get();
    root->setWidth(4.0f);
    root->setHeight(4.0f);
    root->setBackgroundColor({255, 0, 0, 255});
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(app.getPixelBuffer()[0], 0xFFFF0000u);

    app.animate(Motion::tween(1.0f, Easing::linear()), {LayoutMode::Morph}, [&] {
        pointer->setBackgroundColor({0, 0, 255, 255});
    });

    // The final presentation tree is ready immediately, but progress zero must
    // still display the independently owned old raster.
    EXPECT_EQ(pointer->getPresentationBackgroundColor().b, 255);
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(app.getPixelBuffer()[0], 0xFFFF0000u);

    EXPECT_TRUE(app.advanceAnimations(0.5f));
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(app.getPixelBuffer()[0], 0xFF800080u);

    EXPECT_FALSE(app.advanceAnimations(0.5f));
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(app.getPixelBuffer()[0], 0xFF0000FFu);
}

TEST(LclUiTest, MorphFinalUiStopsAnExistingPropertyTrack) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 40, 40, "Morph property interruption");
    auto root = std::make_unique<Container>();
    Container* pointer = root.get();
    root->setWidth(40.0f);
    root->setHeight(40.0f);
    root->setBackgroundColor({255, 0, 0, 255});
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.animate(Motion::tween(1.0f, Easing::linear()), [&] {
        pointer->setBackgroundColor({0, 255, 0, 255});
    });
    ASSERT_TRUE(app.advanceAnimations(0.25f));

    app.animate(Motion::tween(1.0f, Easing::linear()), {LayoutMode::Morph}, [&] {
        pointer->setBackgroundColor({0, 0, 255, 255});
    });
    EXPECT_EQ(pointer->getPresentationBackgroundColor().b, 255);

    ASSERT_TRUE(app.advanceAnimations(0.25f));
    EXPECT_EQ(pointer->getPresentationBackgroundColor().r, 0);
    EXPECT_EQ(pointer->getPresentationBackgroundColor().g, 0);
    EXPECT_EQ(pointer->getPresentationBackgroundColor().b, 255);
}

TEST(LclUiTest, ExplicitKeyframesArePresentationOnlyUntilCommittedAndSurviveCleanup) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 160, 100, "Keyframes");
    auto root = std::make_unique<Container>();
    root->setWidth(160.0f);
    root->setHeight(100.0f);
    Container* pointer = root.get();
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    AnimationOptions options;
    options.durationSec = 1.0f;
    options.fill = FillMode::Forwards;
    auto handle = pointer->animate(AnimatableProperty::Opacity,
        {{0.0f, 1.0f}, {1.0f, 0.0f}}, options);
    app.advanceAnimations(0.5f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.5f, 0.01f);
    EXPECT_FLOAT_EQ(pointer->getOpacity(), 1.0f);
    handle.finish();
    handle.commitFinalStyles();
    EXPECT_FLOAT_EQ(pointer->getOpacity(), 0.0f);

    auto replacement = std::make_unique<Container>();
    replacement->setWidth(160.0f);
    replacement->setHeight(100.0f);
    app.setRootWidget(std::move(replacement));
    EXPECT_NO_THROW(app.advanceAnimations(0.1f));
}

TEST(LclUiTest, ButtonInteractionMotionComposesHoverPressFocusDisabledAndThemeOverride) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 240, 120, "Interaction motion");
    InteractionMotionTheme theme;
    theme.hoverScale = 1.10f;
    app.setInteractionMotionTheme(theme);
    auto root = std::make_unique<Container>();
    root->setWidth(240.0f); root->setHeight(120.0f);
    auto button = std::make_unique<Button>("Motion");
    Button* pointer = button.get();
    button->setWidth(100.0f); button->setHeight(40.0f);
    root->addChild(std::move(button));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    EXPECT_FALSE(app.sendPointerMove(20.0f, 20.0f));
    for (int index = 0; index < 100; ++index) app.advanceAnimations(1.0f / 240.0f);
    EXPECT_EQ(pointer->getState(), ButtonState::Hover);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 1.10f, 0.01f);

    EXPECT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(pointer->getState(), ButtonState::Active);
    EXPECT_TRUE(app.sendPointerUp(20.0f, 20.0f));
    EXPECT_EQ(pointer->getState(), ButtonState::Hover);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), pointer);

    pointer->setEnabled(false);
    EXPECT_EQ(pointer->getState(), ButtonState::Disabled);
    pointer->setEnabled(true);
    EXPECT_EQ(pointer->getState(), ButtonState::Focused);

    theme.enabled = false;
    pointer->setInteractionMotionTheme(theme);
    pointer->setEnabled(false);
    pointer->setEnabled(true);
    EXPECT_FLOAT_EQ(pointer->getPresentationState().scaleX, 1.0f);
}

TEST(LclUiTest, CustomContainerUsesDeclarativeHoverPressedAndClickStates) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 200, 100, "Declarative interaction");
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(100.0f);
    auto control = std::make_unique<Container>();
    Container* pointer = control.get();
    control->setWidth(100.0f);
    control->setHeight(40.0f);
    control->setInteractionStyle(InteractionState::Normal,
        InteractionStyle{.scale = 1.0f, .opacity = 1.0f,
                         .motion = Motion::tween(0.01f, Easing::linear())});
    control->setInteractionStyle(InteractionState::Hover,
        InteractionStyle{.scale = 1.10f, .opacity = 0.90f,
                         .motion = Motion::tween(0.01f, Easing::linear())});
    control->setInteractionStyle(InteractionState::Pressed,
        InteractionStyle{.scale = 0.92f, .opacity = 0.80f,
                         .motion = Motion::tween(0.01f, Easing::linear())});
    int clicks = 0;
    control->setOnClick([&] { ++clicks; });
    root->addChild(std::move(control));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.sendPointerMove(20.0f, 20.0f);
    app.advanceAnimations(0.02f);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 1.10f, 0.001f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.90f, 0.001f);

    EXPECT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), pointer);
    app.advanceAnimations(0.02f);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 0.92f, 0.001f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.80f, 0.001f);

    EXPECT_TRUE(app.sendPointerUp(20.0f, 20.0f));
    EXPECT_EQ(clicks, 1);
    app.advanceAnimations(0.02f);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 1.10f, 0.001f);

    pointer->setInteractionEnabled(false);
    EXPECT_FALSE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(clicks, 1);
}

TEST(LclUiTest, TextUsesStableRasterLayerOnlyWhileAncestorAnimationIsActive) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 240, 120, "Animated text raster layer");
    auto root = std::make_unique<Container>();
    root->setWidth(240.0f);
    root->setHeight(120.0f);
    auto button = std::make_unique<Button>("Stable");
    button->setWidth(100.0f);
    button->setHeight(40.0f);
    root->addChild(std::move(button));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(recorded->texts.size(), 1u);
    EXPECT_TRUE(recorded->rasterTexts.empty());

    app.sendPointerMove(20.0f, 20.0f);
    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(recorded->rasterTexts.size(), 1u);
    EXPECT_EQ(recorded->rasterTexts.back(), "Stable");

    for (int index = 0; index < 300 && app.hasActiveAnimations(); ++index) {
        app.advanceAnimations(1.0f / 240.0f);
    }
    ASSERT_FALSE(app.hasActiveAnimations());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(recorded->texts.size(), 2u);
}

TEST(LclUiTest, WindowAppRendersReplacementRootAfterInitialFrame) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 64, 48, "Replacement root test");

    auto initialRoot = std::make_unique<Container>();
    initialRoot->setBackgroundColor({1, 2, 3, 255});
    initialRoot->getYogaNode().setWidth(64.0f);
    initialRoot->getYogaNode().setHeight(48.0f);
    app.setRootWidget(std::move(initialRoot));
    ASSERT_TRUE(app.renderFrame());
    ASSERT_FALSE(app.renderFrame());

    recorded->rects.clear();
    recorded->colors.clear();

    auto replacementRoot = std::make_unique<Container>();
    replacementRoot->setBackgroundColor({20, 40, 60, 255});
    replacementRoot->getYogaNode().setWidth(64.0f);
    replacementRoot->getYogaNode().setHeight(48.0f);
    app.setRootWidget(std::move(replacementRoot));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(recorded->rects.size(), 1u);
    EXPECT_EQ(recorded->rects.front().width, 64.0f);
    EXPECT_EQ(recorded->rects.front().height, 48.0f);
    ASSERT_EQ(recorded->colors.size(), 1u);
    EXPECT_EQ(recorded->colors.front().r, 20);
    EXPECT_EQ(recorded->colors.front().g, 40);
    EXPECT_EQ(recorded->colors.front().b, 60);
}

TEST(LclUiTest, WindowAppInvokesResizeLifecycleAfterLogicalResize) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Resize callback test");

    uint32_t callbackWidth = 0;
    uint32_t callbackHeight = 0;
    app.setOnResize([&](uint32_t width, uint32_t height) {
        callbackWidth = width;
        callbackHeight = height;
    });

    app.resize(120, 72);

    EXPECT_EQ(app.getWidth(), 120u);
    EXPECT_EQ(app.getHeight(), 72u);
    EXPECT_EQ(callbackWidth, 120u);
    EXPECT_EQ(callbackHeight, 72u);
}

TEST(LclUiTest, AbsoluteEdgePinnedLayerTracksWindowResize) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Edge-pinned resize test");

    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(64.0f);
    root->getYogaNode().setHeight(48.0f);
    auto layer = std::make_unique<Container>();
    Container* layerPointer = layer.get();
    layer->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    layer->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    layer->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    layer->getYogaNode().setPosition(YGEdgeRight, 0.0f);
    layer->getYogaNode().setPosition(YGEdgeBottom, 0.0f);
    root->addChild(std::move(layer));
    app.setRootWidget(std::move(root));

    app.updateLayout();
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().width, 64.0f);
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().height, 48.0f);

    app.resize(120, 72);
    app.updateLayout();
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().width, 120.0f);
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().height, 72.0f);
}

TEST(LclUiTest, WindowAppStagesSurfaceChromeBeforeCompositorConnection) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Staged chrome test");

    EXPECT_TRUE(app.setDecorationMode(lcl::protocol::LCLDecorationMode::CSD));
    EXPECT_TRUE(app.setEdgeToEdge(true));
    EXPECT_TRUE(app.setWindowCornerRadius(14.0f));
}

TEST(LclUiTest, WindowAppSendsExplicitEdgeToEdgeState) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Edge-to-edge test");
    app.setSurfaceId(5);
    app.setExternalIpcSocket(sockets[0]);

    ASSERT_TRUE(app.setEdgeToEdge(true));

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::SetEdgeToEdge);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgSetEdgeToEdge));
    const auto* edgeToEdge =
        reinterpret_cast<const lcl::protocol::LCLMsgSetEdgeToEdge*>(
            payload.data());
    EXPECT_EQ(edgeToEdge->surfaceId, 5u);
    EXPECT_EQ(edgeToEdge->enabled, 1u);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, WindowAppSendsOneWindowCornerStyle) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Corner style test");
    app.setSurfaceId(5);
    app.setExternalIpcSocket(sockets[0]);

    ASSERT_TRUE(app.setWindowCornerStyle(20.0f, 3.2f));

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::SetWindowCornerStyle);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgSetWindowCornerStyle));
    const auto* style = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowCornerStyle*>(payload.data());
    EXPECT_EQ(style->surfaceId, 5u);
    EXPECT_FLOAT_EQ(style->radiusPx, 20.0f);
    EXPECT_FLOAT_EQ(style->roundness, 3.2f);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, WindowAppCsdControlsAndCustomRequestsUseWindowActions) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 540, 360, "Window action test");
    app.setSurfaceId(9);
    app.setExternalIpcSocket(sockets[0]);

    const lcl::ui::chrome::WindowChromeStyle style;
    const auto layout = lcl::ui::chrome::calculateWindowTitlebarLayout(
        540.0f, 34.0f, 20.0f, 14.0f, style);
    app.setCsdTitlebarEnabled(true);
    app.configureCsdTitlebar(34.0f, layout.controlLeft, layout.controlTop,
                             style.controlSize, style.controlGap);

    const auto expectAction = [&](lcl::protocol::LCLWindowAction expected) {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
        EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::RequestWindowAction);
        ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgRequestWindowAction));
        const auto* message = reinterpret_cast<const lcl::protocol::LCLMsgRequestWindowAction*>(payload.data());
        EXPECT_EQ(message->surfaceId, 9u);
        EXPECT_EQ(message->action, expected);
    };

    const auto expectNoAction = [&] {
        char byte = 0;
        errno = 0;
        EXPECT_EQ(recv(sockets[1], &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT), -1);
        EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    };

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft + 1.0f, layout.controlTop + 1.0f));
    expectNoAction();
    EXPECT_TRUE(app.sendPointerUp(layout.controlLeft + 1.0f, layout.controlTop + 1.0f));
    expectAction(lcl::protocol::LCLWindowAction::Close);

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft + style.controlSize + style.controlGap + 1.0f,
                                    layout.controlTop + 1.0f));
    expectNoAction();
    EXPECT_TRUE(app.sendPointerUp(layout.controlLeft + style.controlSize + style.controlGap + 1.0f,
                                  layout.controlTop + 1.0f));
    expectAction(lcl::protocol::LCLWindowAction::Minimize);

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft +
                                        2.0f * (style.controlSize + style.controlGap) + 1.0f,
                                    layout.controlTop + 1.0f));
    expectNoAction();
    EXPECT_TRUE(app.sendPointerUp(layout.controlLeft +
                                      2.0f * (style.controlSize + style.controlGap) + 1.0f,
                                  layout.controlTop + 1.0f));
    expectAction(lcl::protocol::LCLWindowAction::ToggleMaximize);

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft + 1.0f, layout.controlTop + 1.0f));
    EXPECT_TRUE(app.sendPointerUp(300.0f, 100.0f));
    expectNoAction();

    EXPECT_TRUE(app.sendPointerDown(300.0f, 8.0f));
    expectAction(lcl::protocol::LCLWindowAction::BeginDrag);

    EXPECT_TRUE(app.requestWindowRestore());
    expectAction(lcl::protocol::LCLWindowAction::Restore);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, PassiveBackdropSurfaceKeepsItsVisualStateOnPointerEvents) {
    BackdropSurface surface;
    surface.setBackgroundColor({17, 19, 23, 184});
    surface.setInteractive(false);

    PointerEvent event{};
    EXPECT_FALSE(surface.onPointerEnter(event));
    EXPECT_FALSE(surface.onPointerDown(event));
    EXPECT_EQ(surface.getBackgroundColor().r, 17);
    EXPECT_EQ(surface.getBackgroundColor().a, 184);
}

TEST(LclUiTest, GlassUsesTheGenericAddFilterChain) {
    BackdropSurface surface;
    surface.getYogaNode().setWidth(100.0f);
    surface.getYogaNode().setHeight(100.0f);
    surface.setBorderRoundness(3.2f);
    surface.setEffectBounds(EffectBounds::OuterSurface);
    surface.addFilter(lcl::protocol::FilterType::Blur, 8.0f);
    surface.addFilter(lcl::protocol::FilterType::Glass, 30.0f, 3.0f, 12.0f);
    surface.setTint({15, 23, 42, 128});
    surface.getYogaNode().calculateLayout(100.0f, 100.0f);
    surface.syncLayout();

    std::vector<EffectRegion> effects;
    surface.collectEffects(effects);

    ASSERT_EQ(effects.size(), 1u);
    ASSERT_EQ(effects.front().filters.size(), 3u);
    EXPECT_FLOAT_EQ(effects.front().cornerRoundness, 3.2f);
    EXPECT_EQ(effects.front().boundsPolicy, EffectBounds::OuterSurface);
    EXPECT_EQ(effects.front().filters[0].type, lcl::protocol::FilterType::Blur);
    const auto& glass = effects.front().filters[1];
    EXPECT_EQ(glass.type, lcl::protocol::FilterType::Glass);
    EXPECT_FLOAT_EQ(glass.value, 1.0f);
    EXPECT_FLOAT_EQ(glass.params[0], 30.0f);
    EXPECT_FLOAT_EQ(glass.params[1], 3.0f);
    EXPECT_FLOAT_EQ(glass.params[2], 12.0f);
    const auto& tint = effects.front().filters[2];
    EXPECT_EQ(tint.type, lcl::protocol::FilterType::Tint);
    EXPECT_FLOAT_EQ(tint.value, 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(tint.params[0], 15.0f);
    EXPECT_FLOAT_EQ(tint.params[1], 23.0f);
    EXPECT_FLOAT_EQ(tint.params[2], 42.0f);
}

TEST(LclUiTest, PassiveBackdropEffectDoesNotRequireAFullWindowRoundedRaster) {
    RecordingCanvas canvas;
    auto root = std::make_unique<Container>();
    root->setBackgroundColor({17, 19, 23, 184});
    root->getYogaNode().setWidth(540.0f);
    root->getYogaNode().setHeight(360.0f);

    auto effect = std::make_unique<BackdropSurface>();
    effect->setInteractive(false);
    effect->setBorderRadius(20.0f);
    effect->setBorderRoundness(3.2f);
    effect->addFilter(lcl::protocol::FilterType::Blur, 3.5f);
    effect->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    effect->getYogaNode().setWidth(540.0f);
    effect->getYogaNode().setHeight(360.0f);
    root->addChild(std::move(effect));

    root->getYogaNode().calculateLayout(540.0f, 360.0f);
    root->syncLayout();
    root->draw(canvas, {0.0f, 0.0f, 540.0f, 360.0f});

    EXPECT_EQ(canvas.rects.size(), 1u);
    EXPECT_TRUE(canvas.roundedRects.empty());

    std::vector<EffectRegion> effects;
    root->collectEffects(effects);
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_EQ(effects.front().source, EffectSource::Backdrop);
    EXPECT_EQ(effects.front().cornerRadius, 20.0f);
    EXPECT_FLOAT_EQ(effects.front().cornerRoundness, 3.2f);
}

TEST(LclUiTest, SkiaCanvasInjectionPreservesRasterOutput) {
    WindowApp app(lcl::render::makeSkiaCanvas(), 8, 8, "Skia Canvas Test");
    auto root = std::make_unique<Container>();
    root->setBackgroundColor({11, 22, 33, 255});
    root->getYogaNode().setWidth(8.0f);
    root->getYogaNode().setHeight(8.0f);
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_NE(app.getPixelBuffer(), nullptr);
    EXPECT_EQ(app.getPixelBuffer()[0], 0xFF0B1621u);
}

TEST(LclUiTest, RenderPassDamageRect) {
    RenderPass pass;
    EXPECT_FALSE(pass.hasDamage());

    pass.addDirtyRect(Rect{10.0f, 10.0f, 20.0f, 20.0f});
    pass.addDirtyRect(Rect{50.0f, 50.0f, 20.0f, 20.0f});

    EXPECT_TRUE(pass.hasDamage());
    Rect damage = pass.getDamageRect();
    EXPECT_EQ(damage.x, 10.0f);
    EXPECT_EQ(damage.y, 10.0f);
    EXPECT_EQ(damage.width, 60.0f);
    EXPECT_EQ(damage.height, 60.0f);

    pass.clear();
    EXPECT_FALSE(pass.hasDamage());
}

TEST(LclUiTest, YogaNodeFlexLayout) {
    YogaNode root;
    root.setDirection(YGFlexDirectionColumn);
    root.setWidth(200.0f);
    root.setHeight(400.0f);
    root.setPadding(YGEdgeAll, 10.0f);

    YogaNode child1;
    child1.setHeight(50.0f);
    root.appendChild(&child1);

    YogaNode child2;
    child2.setFlexGrow(1.0f);
    root.appendChild(&child2);

    root.calculateLayout(200.0f, 400.0f);

    EXPECT_EQ(child1.getLayoutY(), 10.0f);
    EXPECT_EQ(child1.getLayoutHeight(), 50.0f);

    EXPECT_EQ(child2.getLayoutY(), 60.0f);
    EXPECT_EQ(child2.getLayoutHeight(), 330.0f); // 400 - 20 (padding) - 50 = 330
}

TEST(LclUiTest, WidgetTreeHierarchy) {
    RenderPass pass;
    auto root = std::make_unique<Container>();
    root->setRenderPass(&pass);
    root->getYogaNode().setWidth(300.0f);
    root->getYogaNode().setHeight(300.0f);

    auto btn = std::make_unique<Button>("Click Me");
    Button* btnPtr = btn.get();
    root->addChild(std::move(btn));

    root->getYogaNode().calculateLayout(300.0f, 300.0f);
    root->syncLayout(0.0f, 0.0f);

    EXPECT_EQ(root->getBounds().width, 300.0f);
    EXPECT_EQ(btnPtr->getLabel(), "Click Me");
}

TEST(LclUiTest, RenderPassPropagatesToExistingDescendants) {
    RenderPass pass;
    auto root = std::make_unique<Container>();
    auto child = std::make_unique<Container>();
    Container* childPtr = child.get();
    child->getYogaNode().setWidth(20.0f);
    child->getYogaNode().setHeight(20.0f);
    root->addChild(std::move(child));
    root->getYogaNode().setWidth(100.0f);
    root->getYogaNode().setHeight(100.0f);
    root->getYogaNode().calculateLayout(100.0f, 100.0f);
    root->syncLayout();
    root->setRenderPass(&pass);
    pass.clear();

    childPtr->markDirty();
    EXPECT_TRUE(pass.hasDamage());
}

TEST(LclUiTest, ButtonStateAndClick) {
    auto btn = std::make_unique<Button>("Submit");
    btn->getYogaNode().setWidth(100.0f);
    btn->getYogaNode().setHeight(40.0f);
    btn->getYogaNode().calculateLayout(100.0f, 40.0f);
    btn->syncLayout(0.0f, 0.0f);

    bool clicked = false;
    btn->setOnClick([&clicked]() {
        clicked = true;
    });

    EXPECT_EQ(btn->getState(), ButtonState::Normal);

    // Hover
    PointerEvent moveEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Enter};
    btn->onPointerEnter(moveEv);
    EXPECT_EQ(btn->getState(), ButtonState::Hover);

    // Mouse Down
    PointerEvent downEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down};
    btn->onPointerDown(downEv);
    EXPECT_EQ(btn->getState(), ButtonState::Active);

    // Mouse Up -> triggers click
    PointerEvent upEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up};
    btn->onPointerUp(upEv);
    EXPECT_TRUE(clicked);
    EXPECT_EQ(btn->getState(), ButtonState::Hover);
}

TEST(LclUiTest, RendererMapsLogicalCoordinatesToFractionalBufferPixels) {
    std::vector<uint32_t> pixels(6 * 6, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(6, 6, nullptr, pixels.data()));

    renderer.setContentScale(1.5f);
    renderer.drawRect({1.0f, 1.0f, 2.0f, 2.0f}, {255, 0, 0, 255});

    // Logical [1, 3) maps to physical [1.5, 4.5), i.e. the 3x3 raster area
    // bounded by integer pixels [1, 4). This is the same transform used by
    // WindowApp's 1.5x shared-memory surface.
    EXPECT_EQ(pixels[1 + 1 * 6], 0xFFFF0000u);
    EXPECT_EQ(pixels[3 + 3 * 6], 0xFFFF0000u);
    EXPECT_EQ(pixels[4 + 4 * 6], 0x00000000u);
}

TEST(LclUiTest, RendererMapsLogicalSubtreeToPhysicalOrigin) {
    std::vector<uint32_t> pixels(12 * 12, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(12, 12, nullptr, pixels.data()));

    renderer.setContentScale(1.5f);
    renderer.setContentOrigin(3.0f, 2.0f);
    renderer.drawRect({0.0f, 0.0f, 2.0f, 2.0f}, {0, 255, 0, 255});

    // The 2x2 logical rect becomes 3x3 physical pixels at its physical origin.
    EXPECT_EQ(pixels[3 + 2 * 12], 0xFF00FF00u);
    EXPECT_EQ(pixels[5 + 4 * 12], 0xFF00FF00u);
    EXPECT_EQ(pixels[6 + 5 * 12], 0x00000000u);
}

TEST(LclUiTest, TitlebarRadiusMatchesWindowMaskByDefault) {
    const auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        400.0f, 32.0f, 20.0f, "Window", 15.0f);
    const auto& children = titleBar->getChildren();
    ASSERT_GE(children.size(), 1u);

    const auto* roundedBackground = dynamic_cast<const Container*>(children[0].get());
    ASSERT_NE(roundedBackground, nullptr);
    EXPECT_FLOAT_EQ(roundedBackground->getBorderRadius(), 20.0f);
    EXPECT_TRUE(roundedBackground->hasTopOnlyBorderRadius());
}

TEST(LclUiTest, TitlebarLayoutIsSharedByCsdCloseHitGeometry) {
    const lcl::ui::chrome::WindowChromeStyle style;
    const auto layout = lcl::ui::chrome::calculateWindowTitlebarLayout(
        540.0f, 34.0f, 20.0f, 14.0f, style);

    // The caller gives these same values to WindowApp for its CSD close hit
    // target, while SSD draws the first control through this builder.
    EXPECT_FLOAT_EQ(layout.controlLeft, 12.0f);
    EXPECT_FLOAT_EQ(layout.controlTop, 12.0f);

    auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        540.0f, 34.0f, 20.0f, "LCL Terminal", 14.0f, style);
    titleBar->getYogaNode().calculateLayout(540.0f, 34.0f);
    titleBar->syncLayout();

    const auto& children = titleBar->getChildren();
    ASSERT_GE(children.size(), 4u);
    EXPECT_FLOAT_EQ(children[1]->getBounds().x, layout.controlLeft);
    EXPECT_FLOAT_EQ(children[1]->getBounds().y, layout.controlTop);
    EXPECT_FLOAT_EQ(children[1]->getBounds().width, style.controlSize);
    EXPECT_FLOAT_EQ(children[1]->getBounds().height, style.controlSize);
}

TEST(LclUiTest, WindowControlsAreGlyphFreeAndAnimateHoverPress) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 240, 40, "Window controls");
    const lcl::ui::chrome::WindowChromeStyle style;
    const auto layout = lcl::ui::chrome::calculateWindowTitlebarLayout(
        240.0f, 40.0f, 20.0f, 14.0f, style);
    auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        240.0f, 40.0f, 20.0f, "Window", 14.0f, style);
    auto* closeControl = dynamic_cast<lcl::ui::chrome::WindowControl*>(
        titleBar->getChildren()[1].get());
    ASSERT_NE(closeControl, nullptr);
    EXPECT_TRUE(closeControl->getChildren().empty());

    app.setRootWidget(std::move(titleBar));
    app.configureCsdTitlebar(40.0f, layout.controlLeft, layout.controlTop,
                            style.controlSize, style.controlGap);
    ASSERT_TRUE(app.renderFrame());

    const float controlX = layout.controlLeft + style.controlSize * 0.5f;
    const float controlY = layout.controlTop + style.controlSize * 0.5f;
    app.sendPointerMove(controlX, controlY);
    for (int index = 0; index < 60; ++index) app.advanceAnimations(1.0f / 240.0f);
    EXPECT_GT(closeControl->getPresentationState().scaleX, 1.0f);

    EXPECT_TRUE(app.sendPointerDown(controlX, controlY));
    for (int index = 0; index < 60; ++index) app.advanceAnimations(1.0f / 240.0f);
    EXPECT_LT(closeControl->getPresentationState().scaleX, 1.0f);

    EXPECT_TRUE(app.sendPointerUp(controlX, controlY));
    for (int index = 0; index < 90; ++index) app.advanceAnimations(1.0f / 240.0f);
    EXPECT_GT(closeControl->getPresentationState().scaleX, 1.0f);
}

TEST(LclUiTest, TopRoundedRectDoesNotLeakBelowItsCornerArc) {
    std::vector<uint32_t> pixels(64 * 64, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(64, 64, nullptr, pixels.data()));

    // 20px radius in a 32px-tall titlebar must keep its full outer radius.
    // A normal rounded rect clamps to 16px, while a patched flat-bottom strip
    // paints the pixel that should remain outside this top corner curve.
    renderer.drawTopRoundedRect({0.0f, 0.0f, 50.0f, 32.0f}, 20.0f,
                                {10, 20, 30, 255});

    EXPECT_EQ(pixels[0 + 13 * 64], 0x00000000u);
    EXPECT_EQ(pixels[0 + 21 * 64], 0xFF0A141Eu);
}

TEST(LclUiTest, RoundedRectPreservesTranslucentAlphaOnTransparentCanvas) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));

    renderer.drawRoundedRect({0.0f, 0.0f, 32.0f, 32.0f}, 8.0f,
                             {17, 19, 23, 184}, {}, 0.0f);

    EXPECT_EQ(pixels[16 + 16 * 32], 0xB8111317u);
}

TEST(LclUiTest, StraightAlphaBufferCompositesSourceAlphaAtFullGlobalOpacity) {
    std::vector<uint32_t> pixels(1, 0xFF0000FFu);
    const uint32_t source = 0x66FF0000u;
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(1, 1, nullptr, pixels.data()));

    renderer.drawBuffer(0, 0, 1, 1, &source, 1, 1.0f);

    EXPECT_EQ(pixels[0], 0xFF660099u);
}

TEST(LclUiTest, MaskedAndUnmaskedStraightAlphaBuffersMatchAtTheirCenters) {
    const uint32_t source = 0x6690C0F0u;
    const auto centerPixel = [&](float radius) {
        std::vector<uint32_t> pixels(5 * 5, 0xFF102030u);
        lcl::render::SkiaRenderer renderer;
        EXPECT_TRUE(renderer.initialize(5, 5, nullptr, pixels.data()));
        renderer.drawBuffer(0, 0, 1, 1, &source, 1, 1.0f, radius, 2.0f,
                            false, 5, 5);
        return pixels[2 + 2 * 5];
    };

    EXPECT_EQ(centerPixel(2.0f), centerPixel(0.0f));
}

TEST(LclUiTest, StraightAlphaLayersAccumulateAlphaWithoutSquaringIt) {
    std::vector<uint32_t> pixels(1, 0x00000000u);
    const uint32_t red = 0x80FF0000u;
    const uint32_t green = 0x8000FF00u;
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(1, 1, nullptr, pixels.data()));

    renderer.drawBuffer(0, 0, 1, 1, &red, 1, 1.0f);
    renderer.drawBuffer(0, 0, 1, 1, &green, 1, 1.0f);

    EXPECT_EQ((pixels[0] >> 24) & 0xFFu, 192u);
    EXPECT_EQ((pixels[0] >> 16) & 0xFFu, 85u);
    EXPECT_EQ((pixels[0] >> 8) & 0xFFu, 170u);
    EXPECT_EQ(pixels[0] & 0xFFu, 0u);
}

TEST(LclUiTest, BackdropBlurCaptureExpandsAndClampsWithoutChangingOutputRect) {
    const int radius = lcl::render::gaussianKernelRadius(15.0f);
    EXPECT_EQ(radius, 23);

    const auto centered = lcl::render::computeBackdropFilterGeometry(
        100, 50, 200, 32, 1920, 1080, radius);
    EXPECT_EQ(centered.effect.x, 100);
    EXPECT_EQ(centered.effect.y, 50);
    EXPECT_EQ(centered.effect.width, 200);
    EXPECT_EQ(centered.effect.height, 32);
    EXPECT_EQ(centered.capture.x, 77);
    EXPECT_EQ(centered.capture.y, 27);
    EXPECT_EQ(centered.capture.width, 246);
    EXPECT_EQ(centered.capture.height, 78);
    EXPECT_EQ(centered.outputOffsetX, 23);
    EXPECT_EQ(centered.outputOffsetY, 23);

    const auto screenEdge = lcl::render::computeBackdropFilterGeometry(
        0, 0, 1920, 32, 1920, 1080, radius);
    EXPECT_EQ(screenEdge.effect.x, 0);
    EXPECT_EQ(screenEdge.effect.y, 0);
    EXPECT_EQ(screenEdge.effect.width, 1920);
    EXPECT_EQ(screenEdge.effect.height, 32);
    EXPECT_EQ(screenEdge.capture.x, 0);
    EXPECT_EQ(screenEdge.capture.y, 0);
    EXPECT_EQ(screenEdge.capture.width, 1920);
    EXPECT_EQ(screenEdge.capture.height, 55);
    EXPECT_EQ(screenEdge.outputOffsetX, 0);
    EXPECT_EQ(screenEdge.outputOffsetY, 0);
}

TEST(LclUiTest, SoftwareBackdropPathSkipsBlur) {
    std::vector<uint32_t> pixels{
        0xFF102030u, 0xFF405060u,
        0xFF708090u, 0xFFA0B0C0u,
    };
    const auto original = pixels;
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(2, 2, nullptr, pixels.data()));

    renderer.applyBackdropFilter(
        0, 0, 2, 2, 0.0f, 2.0f, 1.0f,
        {{lcl::protocol::FilterType::Blur, 16.0f}});

    EXPECT_EQ(pixels, original);
}

TEST(LclUiTest, SoftwareBackdropTintUsesTheSameFilteredMaterialPass) {
    std::vector<uint32_t> pixels{0xFF8090A0u};
    lcl::protocol::FilterOp tint{};
    tint.type = lcl::protocol::FilterType::Tint;
    tint.value = 0.5f;
    tint.params[0] = 16.0f;
    tint.params[1] = 32.0f;
    tint.params[2] = 48.0f;

    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(1, 1, nullptr, pixels.data()));
    renderer.applyBackdropFilter(0, 0, 1, 1, 0.0f, 2.0f, 1.0f, {tint});

    EXPECT_EQ(pixels[0], 0xFF485868u);
}

TEST(LclUiTest, SoftwareBackdropMaskUsesTheEffectRoundness) {
    const std::vector<lcl::protocol::FilterOp> filters{
        {lcl::protocol::FilterType::Brightness, 1.0f},
    };

    std::vector<uint32_t> circularPixels(16 * 16, 0xFF102030u);
    lcl::render::SkiaRenderer circularRenderer;
    ASSERT_TRUE(circularRenderer.initialize(16, 16, nullptr, circularPixels.data()));
    circularRenderer.applyBackdropFilter(0, 0, 16, 16, 6.0f, 2.0f, 1.0f, filters);

    std::vector<uint32_t> superellipsePixels(16 * 16, 0xFF102030u);
    lcl::render::SkiaRenderer superellipseRenderer;
    ASSERT_TRUE(superellipseRenderer.initialize(16, 16, nullptr, superellipsePixels.data()));
    superellipseRenderer.applyBackdropFilter(0, 0, 16, 16, 6.0f, 8.0f, 1.0f, filters);

    EXPECT_EQ(circularPixels[1 + 1 * 16] >> 24, 0u);
    EXPECT_EQ(superellipsePixels[1 + 1 * 16] >> 24, 0xFFu);
}

TEST(LclUiTest, RoundedRectPreservesSubpixelEdgeCoverageDuringScaleMotion) {
    const auto edgeAlphaAt = [](float x) {
        std::vector<uint32_t> pixels(16 * 16, 0x00000000u);
        lcl::render::SkiaRenderer renderer;
        EXPECT_TRUE(renderer.initialize(16, 16, nullptr, pixels.data()));
        renderer.drawRoundedRect({x, 2.0f, 8.0f, 8.0f}, 2.0f,
                                 {255, 255, 255, 255}, {}, 0.0f);
        return static_cast<uint8_t>((pixels[0 + 6 * 16] >> 24) & 0xFFu);
    };

    const uint8_t quarterPixel = edgeAlphaAt(0.25f);
    const uint8_t halfPixel = edgeAlphaAt(0.50f);
    EXPECT_GT(quarterPixel, 0u);
    EXPECT_LT(quarterPixel, 255u);
    EXPECT_GT(halfPixel, 0u);
    EXPECT_LT(halfPixel, quarterPixel);
}
