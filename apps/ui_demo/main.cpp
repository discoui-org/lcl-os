#include <iostream>
#include <memory>
#include <string>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "render/skia_canvas.hpp"

using namespace lcl::ui;

namespace {

class DemoButton : public Container {
public:
    explicit DemoButton(const std::string& label) {
        setFocusable(true);
        getYogaNode().setDirection(YGFlexDirectionRow);
        getYogaNode().setJustifyContent(YGJustifyCenter);
        getYogaNode().setAlignItems(YGAlignCenter);
        getYogaNode().setPadding(YGEdgeHorizontal, 16.0f);
        getYogaNode().setPadding(YGEdgeVertical, 8.0f);

        auto labelWidget = std::make_unique<Text>(label);
        labelWidget->setFontSize(15.0f);
        labelWidget->setTextColor(Color{236, 239, 244, 255});
        m_label = labelWidget.get();
        addChild(std::move(labelWidget));

        setBackgroundColor(Color{36, 42, 52, 255});
        setBorderColor(Color{86, 95, 112, 255});
        setBorderWidth(1.0f);
        setBorderRadius(10.0f);
        setOpacity(1.0f);
        setInteractionStyle(InteractionState::Normal,
            InteractionStyle{.scale = 1.0f, .opacity = 1.0f, .motion = std::nullopt});
        setInteractionStyle(InteractionState::Hover,
            InteractionStyle{.scale = 1.015f, .opacity = 1.0f, .motion = std::nullopt});
        setInteractionStyle(InteractionState::Pressed,
            InteractionStyle{.scale = 0.965f, .opacity = 0.92f, .motion = std::nullopt});
    }

    void setLabel(const std::string& text) {
        if (m_label) m_label->setText(text);
    }

private:
    Text* m_label{nullptr};
};

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "  LCL OS - lcl-ui ScrollView Live Demo  \n";
    std::cout << "========================================\n";

    // 1. Initialize WindowApp Application Pipeline (800x600)
    WindowApp app(
        lcl::render::makeSkiaCanvas(), 800, 600,
        "LCL-UI ScrollView Interactive Demo App");
    app.setAppId("org.lcl.uidemo");
    app.setWindowCornerStyle(20.0f, 2.0f);
    app.setEdgeToEdge(true);

    // 2. Build Centered Flexbox Layout Tree in User-Space App
    auto rootContainer = std::make_unique<Container>();
    rootContainer->getYogaNode().setWidth(800.0f);
    rootContainer->getYogaNode().setHeight(600.0f);

    auto backdrop = std::make_unique<BackdropSurface>();
    backdrop->setInteractive(false);
    // The material fills the outer surface behind system insets. Widget
    // content remains in the compositor-provided safe content rect.
    backdrop->setEffectBounds(EffectBounds::OuterSurface);
    backdrop->addFilter(lcl::protocol::FilterType::Blur, 15.0f);
    backdrop->addFilter(lcl::protocol::FilterType::Saturation, 1.4f);
    backdrop->addFilter(lcl::protocol::FilterType::Brightness, 1.1f);
    backdrop->setTint(Color{15, 23, 42, 128});
    backdrop->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    backdrop->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    backdrop->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    backdrop->getYogaNode().setPosition(YGEdgeRight, 0.0f);
    backdrop->getYogaNode().setPosition(YGEdgeBottom, 0.0f);

    auto content = std::make_unique<Container>();
    content->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    content->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    content->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    content->getYogaNode().setPosition(YGEdgeRight, 0.0f);
    content->getYogaNode().setPosition(YGEdgeBottom, 0.0f);
    content->getYogaNode().setDirection(YGFlexDirectionColumn);
    content->getYogaNode().setJustifyContent(YGJustifyCenter);
    content->getYogaNode().setAlignItems(YGAlignCenter);
    content->getYogaNode().setGap(YGGutterAll, 16.0f);

    // Main Card Container
    auto cardContainer = std::make_unique<Container>();
    cardContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
    cardContainer->getYogaNode().setAlignItems(YGAlignCenter);
    cardContainer->getYogaNode().setPadding(YGEdgeAll, 20.0f);
    cardContainer->getYogaNode().setGap(YGGutterAll, 12.0f);
    cardContainer->setBackgroundColor(Color{26, 30, 36, 255});
    cardContainer->setBorderColor(Color{60, 68, 80, 180});
    cardContainer->setBorderWidth(1.0f);
    cardContainer->setBorderRadius(16.0f);

    // Title & Status Label
    auto titleText = std::make_unique<Text>("ScrollView Test Paneli");
    titleText->setFontSize(18.0f);
    titleText->setTextColor(Color{240, 244, 250, 255});

    auto statusText = std::make_unique<Text>("Seçilen: Henüz yok (Tıklama: 0)");
    Text* textPtr = statusText.get();
    statusText->setFontSize(14.0f);
    statusText->setTextColor(Color{160, 174, 192, 255});

    // ScrollView Viewport (340x280)
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->getYogaNode().setWidth(340.0f);
    scrollView->getYogaNode().setHeight(280.0f);

    // Scrollable Content Container
    auto scrollContent = std::make_unique<Container>();
    scrollContent->getYogaNode().setDirection(YGFlexDirectionColumn);
    scrollContent->getYogaNode().setGap(YGGutterAll, 8.0f);
    scrollContent->getYogaNode().setPadding(YGEdgeAll, 8.0f);
    scrollContent->setBackgroundColor(Color{18, 22, 28, 255});
    scrollContent->setBorderColor(Color{45, 52, 64, 255});
    scrollContent->setBorderWidth(1.0f);
    scrollContent->setBorderRadius(12.0f);

    // Add 18 Interactive Row Buttons
    static int clickCounter = 0;
    for (int i = 1; i <= 18; ++i) {
        std::string rowName = "Satır #" + std::to_string(i) + (i % 2 == 0 ? " (Çift)" : " (Tek)");
        auto rowBtn = std::make_unique<DemoButton>(rowName);
        rowBtn->getYogaNode().setHeight(36.0f);

        rowBtn->setOnClick([i, textPtr]() {
            clickCounter++;
            std::string newText = "Seçilen: Satır #" + std::to_string(i) + " (Tıklama: " + std::to_string(clickCounter) + ")";
            textPtr->setText(newText);
            std::cout << "[lcl_ui_demo] Scrolled Row #" << i << " clicked! Total clicks: " << clickCounter << std::endl;
        });

        scrollContent->addChild(std::move(rowBtn));
    }

    scrollView->setContent(std::move(scrollContent));

    cardContainer->addChild(std::move(titleText));
    cardContainer->addChild(std::move(statusText));
    cardContainer->addChild(std::move(scrollView));
    content->addChild(std::move(cardContainer));
    rootContainer->addChild(std::move(backdrop));
    rootContainer->addChild(std::move(content));

    app.setRootWidget(std::move(rootContainer));

    // 3. Connect to Compositor IPC & run live window event loop
    if (app.connectCompositor()) {
        std::cout << "[lcl_ui_demo] App connected to compositor! Running live desktop UI loop...\n";
        app.runEventLoop();
    }

    return 0;
}

