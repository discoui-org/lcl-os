#include <gtest/gtest.h>
#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/core/caret_presentation_controller.hpp"
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
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/text_field.hpp"
#include "render/skia_renderer.hpp"
#include "render/skia_canvas.hpp"
#include "render/backdrop_filter_geometry.hpp"
#include "render/text_metrics.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_set>
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
    void clipRect(const Rect& rect) override { clips.push_back(rect); }
    bool beginCachedLayer(CachedLayerId id, const Rect& bounds) override {
        ++cachedLayerBeginCount;
        activeCachedLayer = id;
        cachedLayerBounds.push_back(bounds);
        return true;
    }
    void endCachedLayer() override {
        ++cachedLayerEndCount;
        cachedLayers.insert(activeCachedLayer);
        activeCachedLayer = 0;
    }
    bool drawCachedLayer(CachedLayerId id, const Rect& destination,
                         float) override {
        if (!cachedLayers.contains(id)) return false;
        ++cachedLayerDrawCount;
        cachedLayerDestinations.push_back(destination);
        return true;
    }

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

    float measureText(const std::string& text, float fontSize, FontFamily family) override {
        if (useSharedTextMetrics) {
            return lcl::render::text_metrics::measureText(text, fontSize, family) *
                measurementScale;
        }
        return static_cast<float>(text.size()) * fontSize * 0.6f * measurementScale;
    }

    void drawBuffer(int, int, int, int, const uint32_t*, int, float,
                    float, float, bool, int, int) override {
        ++bufferDrawCount;
    }

    bool initialized{false};
    bool useSharedTextMetrics{false};
    float measurementScale{1.0f};
    uint32_t* pixels{nullptr};
    uint32_t pixelWidth{0};
    uint32_t pixelHeight{0};
    float contentScale{1.0f};
    int beginCount{0};
    int endCount{0};
    int bufferDrawCount{0};
    int targetSetCount{0};
    int cachedLayerBeginCount{0};
    int cachedLayerEndCount{0};
    int cachedLayerDrawCount{0};
    CachedLayerId activeCachedLayer{0};
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
    std::vector<Rect> clips;
    std::vector<Rect> cachedLayerBounds;
    std::vector<Rect> cachedLayerDestinations;
    std::vector<float> roundedRadii;
    std::vector<float> borderWidths;
    std::vector<float> roundnesses;
    std::vector<float> fontSizes;
    std::vector<FontFamily> fontFamilies;
    std::vector<Color> colors;
    std::vector<Color> borders;
    std::vector<std::string> texts;
    std::vector<std::string> rasterTexts;
    std::unordered_set<CachedLayerId> cachedLayers;
};

class CountingLayoutContainer final : public Container {
public:
    void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f) override {
        ++syncLayoutCount;
        Container::syncLayout(parentAbsX, parentAbsY);
    }

    int syncLayoutCount{0};
};

class CountingPaintWidget final : public Widget {
public:
    void draw(Canvas& canvas, const Rect&) override {
        ++paintCount;
        canvas.drawRect(m_absoluteBounds, {20, 40, 60, 255});
    }

    int paintCount{0};
};

class PointerProbeWidget final : public Widget {
public:
    explicit PointerProbeWidget(Color color = {0, 0, 0, 0}) : m_color(color) {}

    void draw(Canvas& canvas, const Rect&) override {
        ++paintCount;
        canvas.drawRect(m_absoluteBounds, m_color);
    }

    bool onPointerDown(const PointerEvent& event) override {
        ++pointerDownCount;
        if (captureOnDown) event.capturePointer(*this);
        return true;
    }
    bool onPointerUp(const PointerEvent&) override { ++pointerUpCount; return true; }
    bool onPointerCancel(const PointerEvent&) override {
        ++pointerCancelCount;
        if (cancelCountSink) ++*cancelCountSink;
        return true;
    }

    Color m_color;
    bool captureOnDown{false};
    int* cancelCountSink{nullptr};
    int paintCount{0};
    int pointerDownCount{0};
    int pointerUpCount{0};
    int pointerCancelCount{0};
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

TEST(LclUiTest, TextYogaMeasurementMatchesRendererGlyphAdvances) {
    std::vector<uint32_t> pixels(512 * 96, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(512, 96, nullptr, pixels.data()));

    for (const std::string& value : {"iiiiiiii", "WWWWWWWW", "ScrollView Test Paneli"}) {
        Text text(value);
        text.setFontSize(18.0f);
        text.getYogaNode().calculateLayout(512.0f, 96.0f);

        EXPECT_NEAR(text.getYogaNode().getLayoutWidth(), renderer.measureString(value, 18.0f), 0.01f)
            << value;
    }
}

TEST(LclUiTest, FlexCenteredTextUsesItsMeasuredGlyphWidthForOrigin) {
    std::vector<uint32_t> pixels(400 * 96, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(400, 96, nullptr, pixels.data()));

    for (const std::string& value : {"iiiiiiii", "WWWWWWWW", "ScrollView Test Paneli"}) {
        auto root = std::make_unique<Container>();
        root->getYogaNode().setWidth(400.0f);
        root->getYogaNode().setHeight(96.0f);
        root->getYogaNode().setAlignItems(YGAlignCenter);

        auto label = std::make_unique<Text>(value);
        label->setFontSize(18.0f);
        Text* labelPtr = label.get();
        root->addChild(std::move(label));

        root->getYogaNode().calculateLayout(400.0f, 96.0f);
        root->syncLayout();

        const float renderedWidth = renderer.measureString(value, 18.0f);
        EXPECT_NEAR(labelPtr->getAbsoluteBounds().x, (400.0f - renderedWidth) * 0.5f, 0.01f)
            << value;

        RecordingCanvas canvas;
        root->draw(canvas, Rect{0.0f, 0.0f, 400.0f, 96.0f});
        ASSERT_EQ(canvas.textPositions.size(), 1u);
        EXPECT_NEAR(canvas.textPositions.front().x, labelPtr->getAbsoluteBounds().x, 0.01f)
            << value;
    }
}

TEST(LclUiTest, TextMeasurementUpdatesAfterContentAndFamilyChanges) {
    std::vector<uint32_t> pixels(512 * 96, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(512, 96, nullptr, pixels.data()));

    Text text("iiiiiiii");
    text.setFontSize(18.0f);
    text.getYogaNode().calculateLayout(512.0f, 96.0f);
    const float narrowWidth = text.getYogaNode().getLayoutWidth();

    text.setText("WWWWWWWW");
    text.getYogaNode().calculateLayout(512.0f, 96.0f);
    const float wideWidth = text.getYogaNode().getLayoutWidth();
    EXPECT_NE(wideWidth, narrowWidth);
    EXPECT_NEAR(wideWidth, renderer.measureString("WWWWWWWW", 18.0f), 0.01f);

    text.setFontSize(24.0f);
    text.getYogaNode().calculateLayout(512.0f, 96.0f);
    EXPECT_NEAR(text.getYogaNode().getLayoutWidth(), renderer.measureString("WWWWWWWW", 24.0f), 0.01f);

    text.setFontFamily(FontFamily::Monospace);
    text.getYogaNode().calculateLayout(512.0f, 96.0f);
    EXPECT_NEAR(text.getYogaNode().getLayoutWidth(),
                renderer.measureMonospaceString("WWWWWWWW", 24.0f), 0.01f);
}

TEST(LclUiTest, TextFieldPlaceholderAndCaretFollowFocusAndValueState) {
    TextField field;
    field.setPlaceholder("Search");
    field.getYogaNode().setWidth(180.0f);
    field.getYogaNode().calculateLayout(180.0f, 36.0f);
    field.syncLayout();
    const Rect damage{-10.0f, -10.0f, 220.0f, 80.0f};

    RecordingCanvas canvas;
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.back(), "Search");
    EXPECT_TRUE(canvas.rects.empty());

    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    canvas.texts.clear();
    canvas.rects.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.back(), "Search");
    ASSERT_EQ(canvas.rects.size(), 1u);
    EXPECT_FLOAT_EQ(canvas.rects.back().width, 1.0f);

    field.setText("Value");
    canvas.texts.clear();
    canvas.rects.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.back(), "Value");

    field.onFocusLost(FocusEvent{FocusEventType::Lost});
    canvas.rects.clear();
    field.draw(canvas, damage);
    EXPECT_TRUE(canvas.rects.empty());
}

TEST(LclUiTest, CaretPresentationControllerUsesDeterministicHardBlinkPhases) {
    CaretPresentationController caret;
    EXPECT_FALSE(caret.isActive());
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);

    EXPECT_TRUE(caret.setActive(true));
    EXPECT_TRUE(caret.isActive());
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_FALSE(caret.update(0.49f));
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_TRUE(caret.update(0.02f));
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);
    EXPECT_TRUE(caret.update(CaretPresentationController::kHiddenPhaseDurationSec));
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_TRUE(caret.update(CaretPresentationController::kVisiblePhaseDurationSec));
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);

    EXPECT_TRUE(caret.resetActivity());
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_FALSE(caret.update(0.49f));
    EXPECT_TRUE(caret.update(0.02f));
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);

    EXPECT_TRUE(caret.setActive(false));
    EXPECT_FALSE(caret.isActive());
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);
    EXPECT_FALSE(caret.update(1.0f));
}

TEST(LclUiTest, MotionCoordinatorPresentationRegistryTicksOnlyLiveEntries) {
    MotionCoordinator coordinator;
    int firstUpdates = 0;
    int secondUpdates = 0;

    auto first = std::make_unique<Widget>();
    auto second = std::make_unique<Widget>();
    first->setMotionCoordinator(&coordinator);
    second->setMotionCoordinator(&coordinator);
    coordinator.registerPresentation(*first, [&](float) { ++firstUpdates; });
    coordinator.registerPresentation(*second, [&](float) { ++secondUpdates; });

    EXPECT_TRUE(coordinator.hasActiveAnimations());
    EXPECT_TRUE(coordinator.tick(0.1f));
    EXPECT_EQ(firstUpdates, 1);
    EXPECT_EQ(secondUpdates, 1);

    coordinator.unregisterPresentation(second->getObjectId());
    EXPECT_TRUE(coordinator.tick(0.1f));
    EXPECT_EQ(firstUpdates, 2);
    EXPECT_EQ(secondUpdates, 1);

    first.reset();
    EXPECT_FALSE(coordinator.hasActiveAnimations());
    EXPECT_FALSE(coordinator.tick(0.1f));
}

TEST(LclUiTest, TextFieldCaretPresentationResetsForEditingAndCaretActivity) {
    TextField field("abc");
    MotionCoordinator coordinator;
    field.setMotionCoordinator(&coordinator);
    field.getYogaNode().setWidth(180.0f);
    field.getYogaNode().calculateLayout(180.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    const Rect damage{-10.0f, -10.0f, 220.0f, 80.0f};
    RecordingCanvas canvas;

    auto expectVisibleCaret = [&] {
        canvas.rects.clear();
        canvas.colors.clear();
        field.draw(canvas, damage);
        ASSERT_EQ(canvas.rects.size(), 1u);
        ASSERT_FALSE(canvas.colors.empty());
        EXPECT_EQ(canvas.colors.back().a, 255);
    };
    auto expectHiddenCaret = [&] {
        canvas.rects.clear();
        field.draw(canvas, damage);
        EXPECT_TRUE(canvas.rects.empty());
    };

    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onTextInput(TextInputEvent{"ç"}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::ArrowLeft,
                                         static_cast<int>(lcl::platform::PhysicalKey::ArrowLeft)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::ArrowRight,
                                         static_cast<int>(lcl::platform::PhysicalKey::ArrowRight)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                         static_cast<int>(lcl::platform::PhysicalKey::Home)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::End,
                                         static_cast<int>(lcl::platform::PhysicalKey::End)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Backspace,
                                         static_cast<int>(lcl::platform::PhysicalKey::Backspace)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                         static_cast<int>(lcl::platform::PhysicalKey::Home)}));
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Delete,
                                         static_cast<int>(lcl::platform::PhysicalKey::Delete)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onPointerDown(PointerEvent{
        18.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
        PointerSource::Mouse, 0}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    field.setText("reset");
    expectVisibleCaret();
    field.onFocusLost(FocusEvent{FocusEventType::Lost});
    expectHiddenCaret();
    EXPECT_FALSE(coordinator.hasActiveAnimations());
}

TEST(LclUiTest, WindowAppSchedulesCaretPresentationOnlyWhileFieldIsFocused) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 180, 36, "Caret Presentation Test");

    auto field = std::make_unique<TextField>();
    field->setWidth(180.0f);
    TextField* fieldPtr = field.get();
    app.setRootWidget(std::move(field));
    app.updateLayout();

    EXPECT_FALSE(app.hasActiveAnimations());
    ASSERT_TRUE(app.sendPointerDown(10.0f, 10.0f));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), fieldPtr);
    EXPECT_TRUE(app.hasActiveAnimations());
    EXPECT_TRUE(app.advanceAnimations(0.5f));

    recorded->rects.clear();
    fieldPtr->draw(*recorded, {-10.0f, -10.0f, 220.0f, 80.0f});
    EXPECT_TRUE(recorded->rects.empty());

    ASSERT_TRUE(app.sendPointerDown(10.0f, 10.0f, 0, PointerSource::Touch, 7));
    ASSERT_TRUE(app.sendPointerUp(10.0f, 10.0f, 0, PointerSource::Touch, 7));
    recorded->rects.clear();
    recorded->colors.clear();
    fieldPtr->draw(*recorded, {-10.0f, -10.0f, 220.0f, 80.0f});
    ASSERT_EQ(recorded->rects.size(), 1u);
    EXPECT_EQ(recorded->colors.back().a, 255);

    app.getDispatcher().setFocus(nullptr);
    EXPECT_FALSE(app.hasActiveAnimations());
}

TEST(LclUiTest, TextFieldFocusTransferMovesTheActiveCaretPresentation) {
    MotionCoordinator coordinator;
    TextField first("first");
    TextField second("second");
    first.setMotionCoordinator(&coordinator);
    second.setMotionCoordinator(&coordinator);
    for (TextField* field : {&first, &second}) {
        field->getYogaNode().setWidth(180.0f);
        field->getYogaNode().calculateLayout(180.0f, 36.0f);
        field->syncLayout();
    }

    EventDispatcher dispatcher;
    const Rect damage{-10.0f, -10.0f, 220.0f, 80.0f};
    RecordingCanvas canvas;
    dispatcher.setFocus(&first);
    EXPECT_TRUE(coordinator.hasActiveAnimations());
    EXPECT_TRUE(coordinator.tick(0.5f));
    first.draw(canvas, damage);
    EXPECT_TRUE(canvas.rects.empty());

    dispatcher.setFocus(&second);
    canvas.rects.clear();
    second.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    EXPECT_TRUE(coordinator.tick(0.5f));
    canvas.rects.clear();
    second.draw(canvas, damage);
    EXPECT_TRUE(canvas.rects.empty());

    dispatcher.setFocus(nullptr);
    EXPECT_FALSE(coordinator.hasActiveAnimations());
}

TEST(LclUiTest, CaretPhaseChangesProduceDamageButIntermediateTicksDoNot) {
    MotionCoordinator coordinator;
    RenderPass pass;
    TextField field;
    field.setMotionCoordinator(&coordinator);
    field.setRenderPass(&pass);
    field.getYogaNode().setWidth(180.0f);
    field.getYogaNode().calculateLayout(180.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    pass.clear();

    EXPECT_TRUE(coordinator.tick(0.49f));
    EXPECT_FALSE(pass.hasDamage());
    EXPECT_TRUE(coordinator.tick(0.02f));
    EXPECT_TRUE(pass.hasDamage());
    pass.clear();

    EXPECT_TRUE(coordinator.tick(0.1f));
    EXPECT_FALSE(pass.hasDamage());
    EXPECT_TRUE(coordinator.tick(0.4f));
    EXPECT_TRUE(pass.hasDamage());
}

TEST(LclUiTest, TextFieldCaretUsesActiveCanvasProportionalMetrics) {
    TextField field;
    field.getYogaNode().setWidth(240.0f);
    field.getYogaNode().calculateLayout(240.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    const Rect damage{-10.0f, -10.0f, 280.0f, 80.0f};

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    canvas.measurementScale = 0.5f;
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    const float emptyCaretX = canvas.rects.back().x;

    field.setText("Wi");
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::End,
                                         static_cast<int>(lcl::platform::PhysicalKey::End)}));
    canvas.rects.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    const float expectedAdvance = lcl::render::text_metrics::measureText(
        "Wi", 14.0f, FontFamily::Interface) * canvas.measurementScale;
    EXPECT_NEAR(canvas.rects.back().x - emptyCaretX, expectedAdvance, 0.01f);
}

TEST(LclUiTest, TextFieldHorizontallyScrollsLongTextAndClipsCaret) {
    const std::string value = "abcçdefşğıİöüǩžʒ";
    TextField field(value);
    field.getYogaNode().setWidth(56.0f);
    field.getYogaNode().calculateLayout(56.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    field.draw(canvas, {-10.0f, -10.0f, 100.0f, 80.0f});

    ASSERT_FALSE(canvas.clips.empty());
    ASSERT_FALSE(canvas.textPositions.empty());
    ASSERT_EQ(canvas.rects.size(), 1u);
    const Rect viewport = canvas.clips.back();
    const Rect caret = canvas.rects.back();
    EXPECT_LT(canvas.textPositions.back().x, viewport.x);
    EXPECT_GE(caret.x, viewport.x);
    EXPECT_LE(caret.x + caret.width, viewport.x + viewport.width + 0.01f);
    EXPECT_NEAR(caret.x - canvas.textPositions.back().x,
                lcl::render::text_metrics::measureText(
                    value, 14.0f, FontFamily::Interface),
                0.01f);
}

TEST(LclUiTest, TextFieldCaretWalkUsesCodepointPrefixesForAsciiAndUtf8) {
    const std::vector<std::vector<std::string>> prefixSets{
        {"", "a", "ab", "abc", "abcd", "abcde", "abcdef"},
        {"", "a", "ab", "abc", "abcç", "abcçd", "abcçde", "abcçdef"},
        {"", "ş", "şg", "şğı", "şğıİ", "şğıİö", "şğıİöü"},
        {"", "ǩ", "ǩž", "ǩžʒ"},
    };
    const Rect damage{-10.0f, -10.0f, 700.0f, 80.0f};

    for (const auto& prefixes : prefixSets) {
        const std::string& value = prefixes.back();
        SCOPED_TRACE(value);
        TextField field(value);
        field.getYogaNode().setWidth(640.0f);
        field.getYogaNode().calculateLayout(640.0f, 36.0f);
        field.syncLayout();
        field.onFocusGained(FocusEvent{FocusEventType::Gained});
        ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                             static_cast<int>(lcl::platform::PhysicalKey::Home)}));

        RecordingCanvas canvas;
        canvas.useSharedTextMetrics = true;
        for (size_t index = 0; index < prefixes.size(); ++index) {
            SCOPED_TRACE(index);
            canvas.rects.clear();
            canvas.textPositions.clear();
            field.draw(canvas, damage);
            ASSERT_EQ(canvas.rects.size(), 1u);
            ASSERT_EQ(canvas.textPositions.size(), 1u);
            EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                        lcl::render::text_metrics::measureText(
                            prefixes[index], 14.0f, FontFamily::Interface),
                        0.01f);
            if (index + 1 < prefixes.size()) {
                ASSERT_TRUE(field.onKeyDown(KeyEvent{
                    lcl::platform::PhysicalKey::ArrowRight,
                    static_cast<int>(lcl::platform::PhysicalKey::ArrowRight)}));
            }
        }

        for (size_t index = prefixes.size(); index-- > 1;) {
            ASSERT_TRUE(field.onKeyDown(KeyEvent{
                lcl::platform::PhysicalKey::ArrowLeft,
                static_cast<int>(lcl::platform::PhysicalKey::ArrowLeft)}));
            canvas.rects.clear();
            canvas.textPositions.clear();
            field.draw(canvas, damage);
            ASSERT_EQ(canvas.rects.size(), 1u);
            ASSERT_EQ(canvas.textPositions.size(), 1u);
            EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                        lcl::render::text_metrics::measureText(
                            prefixes[index - 1], 14.0f, FontFamily::Interface),
                        0.01f);
        }
    }
}

TEST(LclUiTest, TextFieldPointerCaretPositionUsesUtf8CodepointBoundaries) {
    TextField field("abcçdef");
    field.getYogaNode().setWidth(300.0f);
    field.getYogaNode().calculateLayout(300.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    const Rect damage{-10.0f, -10.0f, 340.0f, 80.0f};

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.textPositions.size(), 1u);
    const float prefixWidth = lcl::render::text_metrics::measureText(
        "abcç", 14.0f, FontFamily::Interface);
    const float pointerX = canvas.textPositions.back().x + prefixWidth;

    ASSERT_TRUE(field.onPointerDown(PointerEvent{
        pointerX, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
        PointerSource::Mouse, 0}));
    canvas.rects.clear();
    canvas.textPositions.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    ASSERT_EQ(canvas.textPositions.size(), 1u);
    EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                prefixWidth, 0.01f);
}

TEST(LclUiTest, TextFieldUtf8InsertionLeavesCaretAfterInsertedCodepoints) {
    TextField field("aç");
    field.getYogaNode().setWidth(300.0f);
    field.getYogaNode().calculateLayout(300.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                         static_cast<int>(lcl::platform::PhysicalKey::Home)}));
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::ArrowRight,
                                         static_cast<int>(lcl::platform::PhysicalKey::ArrowRight)}));
    ASSERT_TRUE(field.onTextInput(TextInputEvent{"ş"}));
    ASSERT_TRUE(field.onTextInput(TextInputEvent{"ğ"}));

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    field.draw(canvas, {-10.0f, -10.0f, 340.0f, 80.0f});
    ASSERT_EQ(canvas.rects.size(), 1u);
    ASSERT_EQ(canvas.textPositions.size(), 1u);
    EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                lcl::render::text_metrics::measureText(
                    "aşğ", 14.0f, FontFamily::Interface),
                0.01f);
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

TEST(LclUiTest, LocalTransientRendersAboveContentInSingleWindowRootLayout) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 64, 48, "Overlay render order");

    auto root = std::make_unique<Container>();
    root->setWidth(64.0f);
    root->setHeight(48.0f);
    root->setBackgroundColor({10, 20, 30, 255});
    root->getYogaNode().setAlignItems(YGAlignCenter);
    root->getYogaNode().setJustifyContent(YGJustifyCenter);
    auto content = std::make_unique<Widget>();
    content->setWidth(10.0f);
    content->setHeight(10.0f);
    Widget* contentPtr = content.get();
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    auto overlay = std::make_unique<PointerProbeWidget>(Color{40, 50, 60, 255});
    overlay->setWidth(24.0f);
    overlay->setHeight(18.0f);
    overlay->setPosition(YGEdgeLeft, 8.0f);
    overlay->setPosition(YGEdgeTop, 6.0f);
    PointerProbeWidget* overlayPtr = overlay.get();
    const TransientHandle handle = app.registerLocalTransient(std::move(overlay));
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(app.renderFrame());
    ASSERT_GE(recorded->colors.size(), 2u);
    EXPECT_EQ(recorded->colors[recorded->colors.size() - 2].r, 10);
    EXPECT_EQ(recorded->colors.back().r, 40);
    EXPECT_FLOAT_EQ(overlayPtr->getAbsoluteBounds().x, 8.0f);
    EXPECT_FLOAT_EQ(overlayPtr->getAbsoluteBounds().y, 6.0f);
    EXPECT_FLOAT_EQ(contentPtr->getAbsoluteBounds().x, 27.0f);
    EXPECT_FLOAT_EQ(contentPtr->getAbsoluteBounds().y, 19.0f);
    EXPECT_FLOAT_EQ(app.getRootWidget()->getAbsoluteBounds().width, 64.0f);
}

TEST(LclUiTest, LocalTransientPrioritizesTopmostHitWithoutBlockingEmptySpace) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay hit testing");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto content = std::make_unique<PointerProbeWidget>();
    content->setWidth(100.0f);
    content->setHeight(80.0f);
    PointerProbeWidget* contentPtr = content.get();
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    auto first = std::make_unique<PointerProbeWidget>();
    first->setWidth(40.0f);
    first->setHeight(30.0f);
    first->setPosition(YGEdgeLeft, 10.0f);
    first->setPosition(YGEdgeTop, 10.0f);
    PointerProbeWidget* firstPtr = first.get();
    const TransientHandle firstHandle = app.registerLocalTransient(std::move(first));

    auto second = std::make_unique<PointerProbeWidget>();
    second->setWidth(40.0f);
    second->setHeight(30.0f);
    second->setPosition(YGEdgeLeft, 10.0f);
    second->setPosition(YGEdgeTop, 10.0f);
    PointerProbeWidget* secondPtr = second.get();
    const TransientHandle secondHandle = app.registerLocalTransient(std::move(second));
    ASSERT_NE(firstHandle, 0u);
    ASSERT_NE(secondHandle, 0u);
    app.updateLayout();

    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(secondPtr->pointerDownCount, 1);
    EXPECT_EQ(firstPtr->pointerDownCount, 0);

    ASSERT_TRUE(app.removeTransient(secondHandle));
    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(firstPtr->pointerDownCount, 1);
    ASSERT_TRUE(app.removeTransient(firstHandle));
    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(contentPtr->pointerDownCount, 1);
}

TEST(LclUiTest, TransientControllerMouseDismissAndRemovalAreLifecycleSafe) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay dismissal");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto content = std::make_unique<PointerProbeWidget>();
    content->setWidth(100.0f);
    content->setHeight(80.0f);
    PointerProbeWidget* contentPtr = content.get();
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    int overlayCancelCount = 0;
    auto insideOverlay = std::make_unique<PointerProbeWidget>();
    insideOverlay->setFocusable(true);
    insideOverlay->captureOnDown = true;
    insideOverlay->cancelCountSink = &overlayCancelCount;
    insideOverlay->setWidth(40.0f);
    insideOverlay->setHeight(30.0f);
    insideOverlay->setPosition(YGEdgeLeft, 10.0f);
    insideOverlay->setPosition(YGEdgeTop, 10.0f);
    PointerProbeWidget* insidePtr = insideOverlay.get();
    const TransientHandle insideHandle = app.registerLocalTransient(
        std::move(insideOverlay), TransientOptions{.dismissOnOutsidePointer = true});
    app.updateLayout();

    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(app.getTransientController().size(), 1u);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), insidePtr);
    EXPECT_TRUE(app.getDispatcher().hasPointerCapture(0, insidePtr));
    ASSERT_TRUE(app.removeTransient(insideHandle));
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_FALSE(app.getDispatcher().hasPointerCapture(0));
    EXPECT_EQ(overlayCancelCount, 1);

    int dismissCount = 0;
    auto outsideOverlay = std::make_unique<PointerProbeWidget>();
    outsideOverlay->setWidth(40.0f);
    outsideOverlay->setHeight(30.0f);
    outsideOverlay->setPosition(YGEdgeLeft, 10.0f);
    outsideOverlay->setPosition(YGEdgeTop, 10.0f);
    app.registerLocalTransient(std::move(outsideOverlay), TransientOptions{
        .dismissOnOutsidePointer = true,
        .onDismiss = [&] { ++dismissCount; },
    });
    app.updateLayout();

    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(dismissCount, 1);
    EXPECT_EQ(contentPtr->pointerDownCount, 1);

    auto nonDismissable = std::make_unique<PointerProbeWidget>();
    nonDismissable->setWidth(20.0f);
    nonDismissable->setHeight(20.0f);
    app.registerLocalTransient(std::move(nonDismissable));
    app.updateLayout();
    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 1u);

    // A non-dismissible topmost overlay blocks dismissal of older entries.
    app.clearTransients();
    app.registerLocalTransient(std::make_unique<PointerProbeWidget>(),
                    TransientOptions{.dismissOnOutsidePointer = true});
    app.registerLocalTransient(std::make_unique<PointerProbeWidget>());
    app.updateLayout();
    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 2u);

    app.clearTransients();
    const TransientHandle lowerHandle = app.registerLocalTransient(std::make_unique<PointerProbeWidget>());
    const TransientHandle topHandle = app.registerLocalTransient(
        std::make_unique<PointerProbeWidget>(), TransientOptions{
            .dismissOnOutsidePointer = true,
            .onDismiss = [&] { app.removeTransient(lowerHandle); },
        });
    ASSERT_NE(lowerHandle, 0u);
    ASSERT_NE(topHandle, 0u);
    app.updateLayout();
    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 0u);
}

TEST(LclUiTest, TransientControllerTouchDismissRequiresValidatedTap) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay touch dismissal");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto content = std::make_unique<PointerProbeWidget>();
    content->setWidth(100.0f);
    content->setHeight(80.0f);
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    auto addDismissable = [&] {
        auto overlay = std::make_unique<PointerProbeWidget>();
        overlay->setWidth(20.0f);
        overlay->setHeight(20.0f);
        app.registerLocalTransient(std::move(overlay), TransientOptions{.dismissOnOutsidePointer = true});
        app.updateLayout();
    };

    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 11));
    EXPECT_EQ(app.getTransientController().size(), 1u);
    ASSERT_TRUE(app.sendPointerUp(80.0f, 60.0f, 0, PointerSource::Touch, 11));
    EXPECT_EQ(app.getTransientController().size(), 0u);

    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 12));
    app.sendPointerMove(81.0f, 61.0f, PointerSource::Touch, 12);
    ASSERT_TRUE(app.sendPointerUp(81.0f, 61.0f, 0, PointerSource::Touch, 12));
    EXPECT_EQ(app.getTransientController().size(), 0u);

    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 13));
    app.sendPointerMove(88.0f, 60.0f, PointerSource::Touch, 13);
    ASSERT_TRUE(app.sendPointerUp(88.0f, 60.0f, 0, PointerSource::Touch, 13));
    EXPECT_EQ(app.getTransientController().size(), 1u);

    app.clearTransients();
    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 14));
    ASSERT_TRUE(app.sendPointerCancel(80.0f, 60.0f, PointerSource::Touch, 14));
    EXPECT_EQ(app.getTransientController().size(), 1u);
}

TEST(LclUiTest, TransientControllerDoesNotDismissForScrollViewTouchGesture) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay scroll gesture");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto scroll = std::make_unique<ScrollView>();
    scroll->setWidth(100.0f);
    scroll->setHeight(80.0f);
    auto content = std::make_unique<Container>();
    content->setWidth(100.0f);
    content->setHeight(240.0f);
    scroll->setContent(std::move(content));
    root->addChild(std::move(scroll));
    app.setRootWidget(std::move(root));

    auto overlay = std::make_unique<PointerProbeWidget>();
    overlay->setWidth(10.0f);
    overlay->setHeight(10.0f);
    app.registerLocalTransient(std::move(overlay), TransientOptions{.dismissOnOutsidePointer = true});
    app.updateLayout();

    app.sendPointerDown(50.0f, 40.0f, 0, PointerSource::Touch, 21);
    ASSERT_TRUE(app.sendPointerMove(50.0f, 52.0f, PointerSource::Touch, 21));
    ASSERT_TRUE(app.sendPointerUp(50.0f, 52.0f, 0, PointerSource::Touch, 21));
    EXPECT_EQ(app.getTransientController().size(), 1u);
}

TEST(LclUiTest, TransientControllerUsesTheSameStableLifecycleForSurfaceEntries) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Surface transient lifecycle");
    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    app.setRootWidget(std::move(root));

    int destroyCount = 0;
    int dismissCount = 0;
    const TransientHandle handle = app.registerSurfaceTransient(
        7, [&] { ++destroyCount; }, TransientOptions{
            .dismissOnOutsidePointer = true,
            .onDismiss = [&] { ++dismissCount; },
        });
    ASSERT_NE(handle, 0u);
    EXPECT_EQ(app.getTransientController().size(), 1u);

    app.sendPointerDown(40.0f, 30.0f);
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(destroyCount, 1);
    EXPECT_EQ(dismissCount, 1);

    const TransientHandle explicitHandle = app.registerSurfaceTransient(
        8, [&] { ++destroyCount; });
    ASSERT_TRUE(app.removeTransient(explicitHandle));
    EXPECT_EQ(destroyCount, 2);
    EXPECT_EQ(dismissCount, 1);
}

TEST(LclUiTest, WindowAppSurfaceTransientRemovalRequestsPopupDestroy) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Popup teardown request");
    app.setExternalIpcSocket(sockets[0]);

    const TransientHandle handle = app.registerSurfaceTransient(7);
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(app.removeTransient(handle));

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, lcl::protocol::LCLOpcode::RequestSurfaceClose);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgRequestSurfaceClose));
    EXPECT_EQ(reinterpret_cast<const lcl::protocol::LCLMsgRequestSurfaceClose*>(
                  payload.data())->surfaceId,
              7u);
    if (receivedFd >= 0) close(receivedFd);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, WindowAppGatesYogaLayoutToLayoutAffectingMutations) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Layout dirty gating");

    auto root = std::make_unique<CountingLayoutContainer>();
    CountingLayoutContainer* rootPointer = root.get();
    root->setWidth(64.0f);
    root->setHeight(48.0f);
    app.setRootWidget(std::move(root));

    EXPECT_TRUE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    EXPECT_EQ(rootPointer->syncLayoutCount, 1);

    rootPointer->markDirty();
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(rootPointer->syncLayoutCount, 1);

    rootPointer->setTranslationY(3.0f);
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(rootPointer->syncLayoutCount, 1);

    rootPointer->getYogaNode().setWidth(60.0f);
    EXPECT_TRUE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    EXPECT_EQ(rootPointer->syncLayoutCount, 2);
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

TEST(LclUiTest, WindowAppCreatesPopupWithParentLocalGeometry) {
    const std::string socketPath = "/tmp/lcl-ui-popup-create-" +
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
    WindowApp popup(std::move(canvas), 120, 70, "Popup");
    popup.setAppId("org.lcl.test.popup");
    popup.setSurfaceId(2);
    popup.configurePopupSurface(1, lcl::protocol::LCLPopupRole::Transient, 250, -8);
    ASSERT_TRUE(popup.connectCompositor(socketPath));

    const int peer = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    ASSERT_GE(peer, 0);
    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(peer, header, payload, receivedFd));
    ASSERT_EQ(header.opcode, lcl::protocol::LCLOpcode::PopupSurfaceCreate);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgPopupSurfaceCreate));
    const auto* request = reinterpret_cast<const lcl::protocol::LCLMsgPopupSurfaceCreate*>(payload.data());
    EXPECT_EQ(request->surfaceId, 2u);
    EXPECT_EQ(request->parentSurfaceId, 1u);
    EXPECT_EQ(request->role, lcl::protocol::LCLPopupRole::Transient);
    EXPECT_EQ(request->x, 250);
    EXPECT_EQ(request->y, -8);
    EXPECT_EQ(request->width, 120u);
    EXPECT_EQ(request->height, 70u);
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

TEST(LclUiTest, BackdropBlurCaptureClampsToSurfaceWithoutPreEntryBleed) {
    const int radius = lcl::render::gaussianKernelRadius(15.0f);
    EXPECT_EQ(radius, 23);

    const auto centered = lcl::render::computeBackdropFilterGeometry(
        100, 50, 200, 32, 1920, 1080, radius);
    EXPECT_EQ(centered.effect.x, 100);
    EXPECT_EQ(centered.effect.y, 50);
    EXPECT_EQ(centered.effect.width, 200);
    EXPECT_EQ(centered.effect.height, 32);
    EXPECT_EQ(centered.capture.x, 100);
    EXPECT_EQ(centered.capture.y, 50);
    EXPECT_EQ(centered.capture.width, 200);
    EXPECT_EQ(centered.capture.height, 32);
    EXPECT_EQ(centered.outputOffsetX, 0);
    EXPECT_EQ(centered.outputOffsetY, 0);

    const auto screenEdge = lcl::render::computeBackdropFilterGeometry(
        0, 0, 1920, 32, 1920, 1080, radius);
    EXPECT_EQ(screenEdge.effect.x, 0);
    EXPECT_EQ(screenEdge.effect.y, 0);
    EXPECT_EQ(screenEdge.effect.width, 1920);
    EXPECT_EQ(screenEdge.effect.height, 32);
    EXPECT_EQ(screenEdge.capture.x, 0);
    EXPECT_EQ(screenEdge.capture.y, 0);
    EXPECT_EQ(screenEdge.capture.width, 1920);
    EXPECT_EQ(screenEdge.capture.height, 32);
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

TEST(LclUiTest, ScrollViewClampsOffsetWhenContentLargerThanViewport) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(300.0f);
    scrollView->setContent(std::move(content));

    scrollView->getYogaNode().calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EXPECT_FLOAT_EQ(scrollView->getContentHeight(), 300.0f);
    EXPECT_FLOAT_EQ(scrollView->getMaxScrollY(), 200.0f);

    scrollView->setScrollY(-50.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);

    scrollView->setScrollY(120.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 120.0f);

    scrollView->setScrollY(500.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 200.0f);
}

TEST(LclUiTest, ScrollViewSkipsDirtyWorkWhenClampedOffsetDoesNotChange) {
    RenderPass pass;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setRenderPass(&pass);
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(300.0f);
    scrollView->setContent(std::move(content));
    scrollView->getYogaNode().calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();
    pass.clear();

    scrollView->setScrollY(-20.0f);
    EXPECT_FALSE(pass.hasDamage());

    scrollView->setScrollY(80.0f);
    EXPECT_TRUE(pass.hasDamage());
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 80.0f);

    pass.clear();
    scrollView->setScrollY(80.0f);
    EXPECT_FALSE(pass.hasDamage());

    scrollView->setScrollY(500.0f);
    EXPECT_TRUE(pass.hasDamage());
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 200.0f);

    pass.clear();
    scrollView->setScrollY(600.0f);
    EXPECT_FALSE(pass.hasDamage());
}

TEST(LclUiTest, ScrollViewCachesContentUntilPaintOrGeometryChanges) {
    RecordingCanvas canvas;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    auto paintedChild = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* paintedChildPointer = paintedChild.get();
    paintedChild->setWidth(200.0f);
    paintedChild->setHeight(300.0f);
    content->addChild(std::move(paintedChild));
    scrollView->setContent(std::move(content));
    scrollView->getYogaNode().calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();

    const Rect fullDamage{-1000.0f, -1000.0f, 4000.0f, 4000.0f};
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 1);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerDrawCount, 1);

    scrollView->setScrollY(40.0f);
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 1);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerDrawCount, 2);
    ASSERT_FALSE(canvas.cachedLayerDestinations.empty());
    EXPECT_FLOAT_EQ(canvas.cachedLayerDestinations.back().y, -40.0f);

    paintedChildPointer->markDirty();
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 2);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 2);

    paintedChildPointer->getYogaNode().setHeight(340.0f);
    scrollView->getYogaNode().calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 3);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 3);

    scrollView->getYogaNode().setHeight(120.0f);
    scrollView->getYogaNode().calculateLayout(200.0f, 120.0f);
    scrollView->syncLayout();
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 4);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 4);
    ASSERT_FALSE(canvas.clips.empty());
    EXPECT_FLOAT_EQ(canvas.clips.back().width, 200.0f);
    EXPECT_FLOAT_EQ(canvas.clips.back().height, 120.0f);
}

TEST(LclUiTest, ScrollViewCachedLayerRespectsViewportClip) {
    std::vector<uint32_t> pixels(64 * 64, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(64, 64, nullptr, pixels.data()));
    lcl::render::SkiaCanvas canvas(renderer);

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(32.0f);
    scrollView->setHeight(16.0f);
    auto content = std::make_unique<Container>();
    content->setWidth(32.0f);
    auto paintedChild = std::make_unique<CountingPaintWidget>();
    paintedChild->setWidth(32.0f);
    paintedChild->setHeight(40.0f);
    content->addChild(std::move(paintedChild));
    scrollView->setContent(std::move(content));
    scrollView->getYogaNode().calculateLayout(32.0f, 16.0f);
    scrollView->syncLayout();

    canvas.beginFrame();
    scrollView->draw(canvas, {-100.0f, -100.0f, 300.0f, 300.0f});
    canvas.endFrame();

    EXPECT_EQ(pixels[4 + 4 * 64], 0xFF14283Cu);
    EXPECT_EQ(pixels[4 + 20 * 64], 0xFF14161Du);
}

TEST(LclUiTest, ScrollViewHandlesWheelScroll) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);
    scrollView->setScrollSpeed(20.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(400.0f);
    scrollView->setContent(std::move(content));

    scrollView->getYogaNode().calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);

    PointerEvent scrollDown{};
    scrollDown.type = PointerEventType::Scroll;
    scrollDown.deltaY = 2.0f;

    EXPECT_TRUE(scrollView->onScroll(scrollDown));
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 40.0f);

    PointerEvent scrollUp{};
    scrollUp.type = PointerEventType::Scroll;
    scrollUp.deltaY = -1.0f;

    EXPECT_TRUE(scrollView->onScroll(scrollUp));
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 20.0f);
}

TEST(LclUiTest, ScrollViewZeroOffsetWhenContentSmallerThanViewport) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(300.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(150.0f);
    scrollView->setContent(std::move(content));

    scrollView->getYogaNode().calculateLayout(200.0f, 300.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EXPECT_FLOAT_EQ(scrollView->getContentHeight(), 150.0f);
    EXPECT_FLOAT_EQ(scrollView->getMaxScrollY(), 0.0f);

    scrollView->setScrollY(100.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);

    PointerEvent scrollEv{};
    scrollEv.type = PointerEventType::Scroll;
    scrollEv.deltaY = 5.0f;
    EXPECT_FALSE(scrollView->onScroll(scrollEv));
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);
}

TEST(LclUiTest, ScrollViewHitTestRoutesToScrolledChild) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(500.0f);

    auto item1 = std::make_unique<Button>("Item 1");
    item1->setWidth(200.0f);
    item1->setHeight(50.0f);

    auto item2 = std::make_unique<Button>("Item 2");
    item2->setWidth(200.0f);
    item2->setHeight(50.0f);
    item2->setPosition(YGEdgeTop, 200.0f);

    Button* item1Ptr = item1.get();
    Button* item2Ptr = item2.get();

    content->addChild(std::move(item1));
    content->addChild(std::move(item2));
    scrollView->setContent(std::move(content));

    scrollView->getYogaNode().calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EventDispatcher dispatcher;

    // At scrollY = 0: Point (50, 25) hits item 1
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 25.0f), item1Ptr);

    // Point (50, 80) is inside ScrollView, but below item1 and above item2 -> hits content container
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 80.0f), scrollView->getContent());

    // Scroll by 200px so item 2 moves into viewport Y: [0, 50]
    scrollView->setScrollY(200.0f);

    // Point (50, 25) now hits item 2
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 25.0f), item2Ptr);

    // Point (50, 225) is physically outside ScrollView viewport (height 100) -> hitTest returns nullptr
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 225.0f), nullptr);
}

TEST(LclUiTest, CanvasClipDiscardsPrimitivesCompletelyOutside) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    lcl::render::SkiaCanvas canvas(renderer);

    canvas.clipRect(Rect{0.0f, 0.0f, 10.0f, 10.0f});
    canvas.drawRect(Rect{15.0f, 15.0f, 10.0f, 10.0f}, Color{255, 255, 255, 255});

    for (uint32_t p : pixels) {
        EXPECT_EQ(p, 0x00000000u);
    }
}

TEST(LclUiTest, CanvasClipClipsPartiallyIntersectingRect) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    lcl::render::SkiaCanvas canvas(renderer);

    canvas.clipRect(Rect{0.0f, 0.0f, 16.0f, 16.0f});
    canvas.drawRect(Rect{0.0f, 0.0f, 32.0f, 32.0f}, Color{255, 255, 255, 255});

    // Inside clip
    EXPECT_EQ(pixels[5 + 5 * 32], 0xFFFFFFFFu);
    EXPECT_EQ(pixels[15 + 15 * 32], 0xFFFFFFFFu);

    // Outside clip
    EXPECT_EQ(pixels[17 + 5 * 32], 0x00000000u);
    EXPECT_EQ(pixels[5 + 17 * 32], 0x00000000u);
    EXPECT_EQ(pixels[20 + 20 * 32], 0x00000000u);
}

TEST(LclUiTest, CanvasClipClipsTextRendering) {
    std::vector<uint32_t> pixels(64 * 64, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(64, 64, nullptr, pixels.data()));
    lcl::render::SkiaCanvas canvas(renderer);

    canvas.clipRect(Rect{0.0f, 0.0f, 32.0f, 20.0f});
    // Draw text outside clip
    canvas.drawText(0.0f, 40.0f, "Out of bounds text", Color{255, 255, 255, 255}, 14.0f, FontFamily::Interface);

    for (int y = 30; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            EXPECT_EQ(pixels[x + y * 64], 0x00000000u);
        }
    }
}

TEST(LclUiTest, CanvasClipSaveAndRestoreRestoresPreviousClip) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::SkiaRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    lcl::render::SkiaCanvas canvas(renderer);

    canvas.clipRect(Rect{0.0f, 0.0f, 24.0f, 24.0f});
    canvas.saveState();
    canvas.clipRect(Rect{0.0f, 0.0f, 8.0f, 8.0f});

    // Draw while inner clip is active
    canvas.drawRect(Rect{12.0f, 12.0f, 4.0f, 4.0f}, Color{255, 0, 0, 255});
    EXPECT_EQ(pixels[13 + 13 * 32], 0x00000000u); // Rejected by inner clip

    canvas.restoreState();

    // After restore, outer clip [0..24, 0..24] is active again
    canvas.drawRect(Rect{12.0f, 12.0f, 4.0f, 4.0f}, Color{0, 255, 0, 255});
    EXPECT_EQ(pixels[13 + 13 * 32], 0xFF00FF00u); // Drawn successfully
}
