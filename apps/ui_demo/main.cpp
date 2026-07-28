#include <iostream>
#include <memory>
#include <string>
#include <cassert>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/text.hpp"

using namespace lcl::ui;

int main() {
    std::cout << "========================================\n";
    std::cout << "  LCL OS - lcl-ui Phase 1.6 Live Demo   \n";
    std::cout << "========================================\n";

    // 1. Initialize WindowApp Application Pipeline (800x600)
    WindowApp app(800, 600, "LCL-UI Phase 1.6 Interactive Demo App");

    // 2. Build Centered Flexbox Layout Tree in User-Space App
    auto rootContainer = std::make_unique<Container>();
    rootContainer->getYogaNode().setWidth(800.0f);
    rootContainer->getYogaNode().setHeight(600.0f);
    rootContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
    rootContainer->getYogaNode().setJustifyContent(YGJustifyCenter);
    rootContainer->getYogaNode().setAlignItems(YGAlignCenter);
    rootContainer->getYogaNode().setGap(YGGutterAll, 20.0f);

    // Inner Card Container
    auto cardContainer = std::make_unique<Container>();
    cardContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
    cardContainer->getYogaNode().setAlignItems(YGAlignCenter);
    cardContainer->getYogaNode().setPadding(YGEdgeAll, 24.0f);
    cardContainer->getYogaNode().setGap(YGGutterAll, 16.0f);

    // Button & Text Widgets
    auto clickButton = std::make_unique<Button>("Tıkla: 0");
    Button* btnPtr = clickButton.get();
    clickButton->getYogaNode().setWidth(160.0f);
    clickButton->getYogaNode().setHeight(44.0f);

    auto statusText = std::make_unique<Text>("Tıklama Sayısı: 0");
    Text* textPtr = statusText.get();
    statusText->setFontSize(16.0f);

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
