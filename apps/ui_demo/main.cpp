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

    // 3. Render Initial Frame
    std::cout << "[lcl_ui_demo] Rendering initial frame...\n";
    bool initialFrameRendered = app.renderFrame();
    std::cout << "[lcl_ui_demo] Initial frame redrawn: " << (initialFrameRendered ? "YES" : "NO") << std::endl;
    assert(initialFrameRendered);

    // 4. Simulate Idle Frame (No damage -> should skip redraw)
    bool idleRendered = app.renderFrame();
    std::cout << "[lcl_ui_demo] Idle frame redrawn: " << (idleRendered ? "YES" : "NO") << std::endl;
    assert(!idleRendered);

    // 5. Simulate Hover Input Pipeline (Move pointer over button center)
    float btnCenterX = btnPtr->getAbsoluteBounds().x + btnPtr->getAbsoluteBounds().width / 2.0f;
    float btnCenterY = btnPtr->getAbsoluteBounds().y + btnPtr->getAbsoluteBounds().height / 2.0f;

    std::cout << "[lcl_ui_demo] Sending PointerMove to (" << btnCenterX << ", " << btnCenterY << ")...\n";
    app.sendPointerMove(btnCenterX, btnCenterY);
    assert(btnPtr->getState() == ButtonState::Hover);

    bool hoverFrameRendered = app.renderFrame();
    std::cout << "[lcl_ui_demo] Hover frame redrawn: " << (hoverFrameRendered ? "YES" : "NO") << std::endl;
    assert(hoverFrameRendered);

    // 6. Simulate Pointer Click Pipeline (Down + Up)
    std::cout << "[lcl_ui_demo] Sending PointerDown & PointerUp to button...\n";
    app.sendPointerDown(btnCenterX, btnCenterY, 0);
    app.sendPointerUp(btnCenterX, btnCenterY, 0);

    bool clickFrameRendered = app.renderFrame();
    std::cout << "[lcl_ui_demo] Click frame redrawn: " << (clickFrameRendered ? "YES" : "NO") << std::endl;
    assert(clickFrameRendered);

    // 7. Verify State Updates
    std::cout << "[lcl_ui_demo] Verification: Click Counter = " << clickCounter << std::endl;
    std::cout << "[lcl_ui_demo] Verification: Button Label = \"" << btnPtr->getLabel() << "\"\n";
    std::cout << "[lcl_ui_demo] Verification: Text Content = \"" << textPtr->getText() << "\"\n";

    assert(clickCounter == 1);
    assert(btnPtr->getLabel() == "Tıkla: 1");
    assert(textPtr->getText() == "Tıklama Sayısı: 1");

    std::cout << "========================================\n";
    std::cout << "  Phase 1.6 Demo Execution Successful! \n";
    std::cout << "========================================\n";

    // 8. Connect to Compositor IPC & run live window event loop
    if (app.connectCompositor()) {
        std::cout << "[lcl_ui_demo] App connected to compositor! Running live desktop UI loop...\n";
        app.runEventLoop();
    }

    return 0;
}
