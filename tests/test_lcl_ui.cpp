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
#include "render/skia_renderer.hpp"

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
        pixels = targetPixels;
        pixelWidth = width;
        pixelHeight = height;
    }

    void setContentScale(float value) override { contentScale = value; }
    void beginFrame() override { ++beginCount; }
    void endFrame() override { ++endCount; }
    uint32_t* rasterBuffer() override { return pixels; }

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
                  float fontSize) override {
        textPositions.push_back({x, y, 0.0f, 0.0f});
        texts.push_back(text);
        colors.push_back(color);
        fontSizes.push_back(fontSize);
    }

    float measureText(const std::string& text, float fontSize) override {
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
    std::vector<Rect> rects;
    std::vector<Rect> roundedRects;
    std::vector<Rect> topRoundedRects;
    std::vector<Rect> textPositions;
    std::vector<float> roundedRadii;
    std::vector<float> borderWidths;
    std::vector<float> roundnesses;
    std::vector<float> fontSizes;
    std::vector<Color> colors;
    std::vector<Color> borders;
    std::vector<std::string> texts;
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

TEST(LclUiTest, WindowAppDefaultCanvasPreservesRasterOutput) {
    WindowApp app(8, 8, "Default Canvas Test");
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
    const auto titleBar = lcl::ui::chrome::buildLibadwaitaTitleBar(
        400.0f, 32.0f, 20.0f, "Window", 15.0f);
    const auto& children = titleBar->getChildren();
    ASSERT_GE(children.size(), 1u);

    const auto* roundedBackground = dynamic_cast<const Container*>(children[0].get());
    ASSERT_NE(roundedBackground, nullptr);
    EXPECT_FLOAT_EQ(roundedBackground->getBorderRadius(), 20.0f);
    EXPECT_TRUE(roundedBackground->hasTopOnlyBorderRadius());
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
