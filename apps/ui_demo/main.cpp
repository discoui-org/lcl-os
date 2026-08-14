#include <iostream>
#include <memory>
#include <string>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/text.hpp"
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
        getYogaNode().setPadding(YGEdgeVertical, 10.0f);

        auto labelWidget = std::make_unique<Text>(label);
        labelWidget->setFontSize(16.0f);
        labelWidget->setTextColor(Color{236, 239, 244, 255});
        m_label = labelWidget.get();
        addChild(std::move(labelWidget));

        setBackgroundColor(Color{36, 42, 52, 255});
        setBorderColor(Color{86, 95, 112, 255});
        setBorderWidth(1.0f);
        setBorderRadius(14.0f);
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
    std::cout << "  LCL OS - lcl-ui Phase 1.6 Live Demo   \n";
    std::cout << "========================================\n";

    // 1. Initialize WindowApp Application Pipeline (800x600)
    WindowApp app(
        lcl::render::makeSkiaCanvas(), 800, 600,
        "LCL-UI Phase 1.6 Interactive Demo App");
    app.setAppId("org.lcl.uidemo");

    // 2. Build Centered Flexbox Layout Tree in User-Space App
    auto rootContainer = std::make_unique<BackdropSurface>();
    rootContainer->setInteractive(false);
    rootContainer->addFilter(lcl::protocol::FilterType::Blur, 50.0f);
    rootContainer->getYogaNode().setWidth(800.0f);
    rootContainer->getYogaNode().setHeight(600.0f);
    rootContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
    rootContainer->getYogaNode().setJustifyContent(YGJustifyCenter);
    rootContainer->getYogaNode().setAlignItems(YGAlignCenter);
    rootContainer->getYogaNode().setGap(YGGutterAll, 20.0f);
    rootContainer->setBackgroundColor(Color{17, 19, 23, 190});

    // Inner Card Container
    auto cardContainer = std::make_unique<Container>();
    cardContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
    cardContainer->getYogaNode().setAlignItems(YGAlignCenter);
    cardContainer->getYogaNode().setPadding(YGEdgeAll, 24.0f);
    cardContainer->getYogaNode().setGap(YGGutterAll, 16.0f);
    cardContainer->setBackgroundColor(Color{26, 30, 36, 255});
    cardContainer->setBorderColor(Color{10, 12, 16, 120});
    cardContainer->setBorderWidth(1.0f);
    cardContainer->setBorderRadius(0.0f);

    // Button & Text Widgets
    auto clickButton = std::make_unique<DemoButton>("Tıkla: 0");
    DemoButton* btnPtr = clickButton.get();
    clickButton->getYogaNode().setWidth(176.0f);
    clickButton->getYogaNode().setHeight(44.0f);

    auto statusText = std::make_unique<Text>("Tıklama Sayısı: 0");
    Text* textPtr = statusText.get();
    statusText->setFontSize(16.0f);
    statusText->setTextColor(Color{196, 202, 211, 255});

    // Application State & Interactive Counter Callback
    static int clickCounter = 0;
    btnPtr->setOnClick([btnPtr, textPtr]() {
        clickCounter++;
        std::string newBtnLabel = "Tıkla: " + std::to_string(clickCounter);
        std::string newText = "Tıklama Sayısı: " + std::to_string(clickCounter);

        btnPtr->setLabel(newBtnLabel);
        textPtr->setText(newText);

        std::cout << "[lcl_ui_demo] Button clicked! User application state updated to: " << newText << std::endl;
    });

    cardContainer->addChild(std::move(clickButton));
    cardContainer->addChild(std::move(statusText));
    rootContainer->addChild(std::move(cardContainer));

    app.setRootWidget(std::move(rootContainer));

    // 3. Connect to Compositor IPC & run live window event loop
    if (app.connectCompositor()) {
        std::cout << "[lcl_ui_demo] App connected to compositor! Running live desktop UI loop...\n";
        app.runEventLoop();
    }

    return 0;
}
