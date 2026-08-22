#include <gtest/gtest.h>
#include "platform/common/native_buffer.hpp"
#include "platform/common/graphics_context.hpp"
#include "platform/common/display_backend.hpp"
#include "platform/common/input_backend.hpp"
#include "platform/common/runtime_paths.hpp"
#include "platform/common/platform_services.hpp"

namespace {

using namespace lcl::platform;

// Mock concrete native buffer
class MockNativeBuffer final : public INativeBuffer {
public:
    MockNativeBuffer(uint32_t w, uint32_t h) : m_width(w), m_height(h) {}
    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
private:
    uint32_t m_width{0};
    uint32_t m_height{0};
};

// Mock concrete graphics context
class MockGraphicsContext final : public IGraphicsContext {
public:
    bool isInitialized() const override { return m_initialized; }
    bool isHardwareAccelerated() const override { return true; }
    bool makeCurrent() override { return true; }
    bool resize(uint32_t w, uint32_t h) override { m_w = w; m_h = h; return true; }
    bool presentsToDisplay() const override { return true; }
    bool present() override { m_presentCount++; return true; }
    bool readback(uint32_t*, uint32_t, uint32_t) override { return false; }

    TextureHandle importTexture(const INativeBuffer& buffer) override {
        return (buffer.width() > 0 && buffer.height() > 0) ? 42 : kInvalidTextureHandle;
    }
    void releaseTexture(TextureHandle texture) override {
        if (texture == 42) m_released = true;
    }

    bool m_initialized{true};
    uint32_t m_w{0}, m_h{0};
    uint32_t m_presentCount{0};
    bool m_released{false};
};

// Mock concrete display backend
class MockDisplayBackend final : public IDisplayBackend {
public:
    bool initialize() override { m_initialized = true; return true; }
    void shutdown() override { m_initialized = false; }
    bool isInitialized() const override { return m_initialized; }
    const DisplayMode& activeMode() const override { return m_mode; }

    bool initHardwareCursor(uint32_t, uint32_t, float = 1.0f) override {
        m_cursorActive = true;
        return true;
    }
    bool moveHardwareCursor(int x, int y) override { m_cursorX = x; m_cursorY = y; return true; }
    bool isHardwareCursorActive() const override { return m_cursorActive; }

    bool m_initialized{false};
    bool m_cursorActive{false};
    int m_cursorX{0}, m_cursorY{0};
    DisplayMode m_mode{1920, 1080, 60, 60, 1.0f, "1080p"};
};

// Mock concrete input backend
class MockInputBackend final : public IInputBackend {
public:
    bool initialize(InputEventCallback callback) override {
        m_callback = std::move(callback);
        m_initialized = true;
        return true;
    }
    void shutdown() override { m_initialized = false; }
    bool isInitialized() const override { return m_initialized; }
    size_t pollEvents(int, int) override {
        if (m_callback) {
            RawInputEvent ev{};
            ev.type = RawInputEventType::PointerMotion;
            ev.dx = 5.0;
            ev.dy = 10.0;
            m_callback(ev);
            return 1;
        }
        return 0;
    }

    bool m_initialized{false};
    InputEventCallback m_callback;
};

// Mock concrete runtime paths
class MockRuntimePaths final : public IRuntimePaths {
public:
    std::string compositorSocketPath() const override { return "/tmp/mock-compositor.sock"; }
    std::string sessionSocketPath() const override { return "/tmp/mock-session.sock"; }
    std::string appCatalogDirectory() const override { return "/tmp/mock-apps"; }
    std::vector<std::string> fontSearchDirectories() const override { return {"/tmp/mock-fonts"}; }
    std::string temporaryDirectory() const override { return "/tmp"; }
};

// Mock concrete platform services container
class MockPlatformServices final : public IPlatformServices {
public:
    bool initialize() override { m_initialized = true; return true; }
    void shutdown() override { m_initialized = false; }
    bool isInitialized() const override { return m_initialized; }

    IDisplayBackend& display() override { return m_display; }
    IGraphicsContext& graphics() override { return m_graphics; }
    IInputBackend& input() override { return m_input; }
    const IRuntimePaths& paths() const override { return m_paths; }

    bool m_initialized{false};
    MockDisplayBackend m_display;
    MockGraphicsContext m_graphics;
    MockInputBackend m_input;
    MockRuntimePaths m_paths;
};

} // namespace

TEST(PlatformInterfacesTest, NativeBufferPolymorphism) {
    MockNativeBuffer buf(1280, 720);
    const INativeBuffer& iface = buf;
    EXPECT_EQ(iface.width(), 1280u);
    EXPECT_EQ(iface.height(), 720u);
}

TEST(PlatformInterfacesTest, GraphicsContextTextureImport) {
    MockGraphicsContext ctx;
    MockNativeBuffer validBuf(800, 600);
    MockNativeBuffer emptyBuf(0, 0);

    TextureHandle h1 = ctx.importTexture(validBuf);
    EXPECT_EQ(h1, 42u);

    TextureHandle h2 = ctx.importTexture(emptyBuf);
    EXPECT_EQ(h2, kInvalidTextureHandle);

    ctx.releaseTexture(h1);
    EXPECT_TRUE(ctx.m_released);
}

TEST(PlatformInterfacesTest, DisplayBackendContract) {
    MockDisplayBackend display;
    EXPECT_FALSE(display.isInitialized());
    EXPECT_TRUE(display.initialize());
    EXPECT_TRUE(display.isInitialized());
    EXPECT_EQ(display.activeMode().width, 1920u);
    EXPECT_EQ(display.activeMode().height, 1080u);

    EXPECT_TRUE(display.initHardwareCursor(64, 64));
    EXPECT_TRUE(display.isHardwareCursorActive());
    EXPECT_TRUE(display.moveHardwareCursor(100, 200));
    EXPECT_EQ(display.m_cursorX, 100);
    EXPECT_EQ(display.m_cursorY, 200);

    display.shutdown();
    EXPECT_FALSE(display.isInitialized());
}

TEST(PlatformInterfacesTest, InputBackendDispatch) {
    MockInputBackend input;
    RawInputEvent receivedEvent{};
    bool called = false;

    EXPECT_TRUE(input.initialize([&](const RawInputEvent& ev) {
        receivedEvent = ev;
        called = true;
    }));

    size_t count = input.pollEvents(1920, 1080);
    EXPECT_EQ(count, 1u);
    EXPECT_TRUE(called);
    EXPECT_EQ(receivedEvent.type, RawInputEventType::PointerMotion);
    EXPECT_DOUBLE_EQ(receivedEvent.dx, 5.0);
    EXPECT_DOUBLE_EQ(receivedEvent.dy, 10.0);
}

TEST(PlatformInterfacesTest, RuntimePathsContract) {
    MockRuntimePaths paths;
    EXPECT_EQ(paths.compositorSocketPath(), "/tmp/mock-compositor.sock");
    EXPECT_EQ(paths.sessionSocketPath(), "/tmp/mock-session.sock");
    EXPECT_EQ(paths.appCatalogDirectory(), "/tmp/mock-apps");
    ASSERT_EQ(paths.fontSearchDirectories().size(), 1u);
    EXPECT_EQ(paths.fontSearchDirectories()[0], "/tmp/mock-fonts");
}

TEST(PlatformInterfacesTest, PlatformServicesComposition) {
    MockPlatformServices services;
    IPlatformServices& iface = services;

    EXPECT_TRUE(iface.display().initialize());
    EXPECT_TRUE(iface.graphics().isHardwareAccelerated());
    EXPECT_EQ(iface.paths().compositorSocketPath(), "/tmp/mock-compositor.sock");
}

#include "platform/desktop/drm_display_backend.hpp"
#include "platform/desktop/gbm_graphics_context.hpp"
#include "platform/desktop/dma_buf_native_buffer.hpp"
#include "platform/desktop/evdev_input_backend.hpp"

TEST(DesktopPlatformTest, DrmDisplayBackendImplementsIDisplayBackend) {
    lcl::platform::desktop::DrmDisplayBackend drmBackend;
    lcl::platform::IDisplayBackend& iface = drmBackend;

    EXPECT_FALSE(iface.isInitialized());
    EXPECT_FALSE(iface.isHardwareCursorActive());
    EXPECT_EQ(drmBackend.getDisplayType(), lcl::platform::desktop::DesktopDisplayType::None);
    EXPECT_EQ(drmBackend.getDrmFd(), -1);
}

TEST(DesktopPlatformTest, GbmGraphicsContextImplementsIGraphicsContext) {
    lcl::platform::desktop::GbmGraphicsContext gbmCtx;
    lcl::platform::IGraphicsContext& iface = gbmCtx;

    EXPECT_FALSE(iface.isInitialized());
    EXPECT_FALSE(iface.isHardwareAccelerated());
    EXPECT_TRUE(iface.presentsToDisplay());
    EXPECT_FALSE(iface.readback(nullptr, 0, 0));
}

TEST(DesktopPlatformTest, DmaBufNativeBufferImplementsINativeBuffer) {
    lcl::platform::desktop::DmaBufNativeBuffer dmaBuf(10, 1920, 1080, 1920 * 4, 1, 0x12345678ULL);
    const lcl::platform::INativeBuffer& iface = dmaBuf;

    EXPECT_EQ(iface.width(), 1920u);
    EXPECT_EQ(iface.height(), 1080u);
    EXPECT_EQ(dmaBuf.fd(), 10);
    EXPECT_EQ(dmaBuf.stride(), 1920u * 4u);
    EXPECT_EQ(dmaBuf.format(), 1u);
    EXPECT_EQ(dmaBuf.modifier(), 0x12345678ULL);
}

TEST(DesktopPlatformTest, EvdevInputBackendImplementsIInputBackend) {
    lcl::platform::desktop::EvdevInputBackend inputBackend;
    lcl::platform::IInputBackend& iface = inputBackend;

    EXPECT_FALSE(iface.isInitialized());
    EXPECT_EQ(inputBackend.getFd(), -1);

    bool callbackCalled = false;
    bool init = iface.initialize([&](const lcl::platform::RawInputEvent&) {
        callbackCalled = true;
    });
    // On host without udevd/input nodes, falls back gracefully or succeeds
    if (init) {
        EXPECT_TRUE(iface.isInitialized());
        iface.pollEvents(1920, 1080);
        iface.shutdown();
        EXPECT_FALSE(iface.isInitialized());
    }
    (void)callbackCalled;
}

#include "platform/desktop/desktop_runtime_paths.hpp"
#include "platform/desktop/desktop_platform_services.hpp"

TEST(DesktopPlatformTest, DesktopRuntimePathsImplementsIRuntimePaths) {
    lcl::platform::desktop::DesktopRuntimePaths paths;
    const lcl::platform::IRuntimePaths& iface = paths;

    EXPECT_EQ(iface.compositorSocketPath(), "/Runtime/lcl-compositor.sock");
    EXPECT_EQ(iface.sessionSocketPath(), "/Runtime/lcl-sessiond.sock");
    EXPECT_EQ(iface.appCatalogDirectory(), "/System/Applications");
    EXPECT_EQ(iface.temporaryDirectory(), "/Runtime/Temporary");
    EXPECT_FALSE(iface.fontSearchDirectories().empty());
}

TEST(DesktopPlatformTest, DesktopPlatformServicesImplementsIPlatformServices) {
    lcl::platform::desktop::DesktopPlatformServices services;
    lcl::platform::IPlatformServices& iface = services;

    EXPECT_FALSE(iface.isInitialized());
    EXPECT_FALSE(iface.display().isInitialized());
    EXPECT_FALSE(iface.graphics().isInitialized());
    EXPECT_FALSE(iface.input().isInitialized());
    EXPECT_EQ(iface.paths().compositorSocketPath(), "/Runtime/lcl-compositor.sock");
}

#include "platform/android/android_input_backend.hpp"

TEST(AndroidPlatformTest, AndroidInputBackendImplementsIInputBackend) {
    lcl::platform::android::AndroidInputBackend inputBackend;
    lcl::platform::IInputBackend& iface = inputBackend;

    EXPECT_FALSE(iface.isInitialized());

    bool callbackCalled = false;
    bool init = iface.initialize([&](const lcl::platform::RawInputEvent&) {
        callbackCalled = true;
    });
    EXPECT_TRUE(init);
    EXPECT_TRUE(iface.isInitialized());

    // Polling without events returns 0 and does not crash
    size_t count = iface.pollEvents(1080, 1920);
    EXPECT_EQ(count, 0u);

    iface.shutdown();
    EXPECT_FALSE(iface.isInitialized());
    (void)callbackCalled;
}

TEST(AndroidPlatformTest, AndroidInputBackendReinitializesWithCallback) {
    lcl::platform::android::AndroidInputBackend inputBackend;
    lcl::platform::IInputBackend& iface = inputBackend;

    // 1. Initial initialization with nullptr (e.g. AndroidPlatformServices::initialize)
    EXPECT_TRUE(iface.initialize(nullptr));
    EXPECT_TRUE(iface.isInitialized());

    // 2. Subsequent initialization with real callback (e.g. Compositor::initialize)
    bool callbackCalled = false;
    EXPECT_TRUE(iface.initialize([&](const lcl::platform::RawInputEvent&) {
        callbackCalled = true;
    }));
    EXPECT_TRUE(iface.isInitialized());

    iface.shutdown();
    EXPECT_FALSE(iface.isInitialized());
    (void)callbackCalled;
}
