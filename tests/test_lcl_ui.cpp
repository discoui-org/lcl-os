#include <gtest/gtest.h>
#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/layout/yoga_node.hpp"
#include "lcl-ui/widgets/widget.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "render/skia_renderer.hpp"

#include <vector>

using namespace lcl::ui;

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
