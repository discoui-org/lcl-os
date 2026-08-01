#include <iostream>
#include <memory>
#include <string>
#include <cassert>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/text.hpp"

using namespace lcl::ui;

namespace {

class BlurButton : public BackdropSurface {
public:
    explicit BlurButton(const std::string& label) {
        setFocusable(true);
        getYogaNode().setDirection(YGFlexDirectionRow);
        getYogaNode().setJustifyContent(YGJustifyCenter);
        getYogaNode().setAlignItems(YGAlignCenter);
        getYogaNode().setPadding(YGEdgeHorizontal, 16.0f);
        getYogaNode().setPadding(YGEdgeVertical, 10.0f);

        m_label = std::make_unique<Text>(label);
        m_label->setFontSize(16.0f);
        m_label->setTextColor(Color{255, 255, 255, 255});
        m_labelPtr = m_label.get();
        addChild(std::move(m_label));

        addFilter(lcl::protocol::FilterType::Blur, 22.0f);
        setBackgroundColor(Color{255, 255, 255, 0});
        setBorderColor(Color{255, 255, 255, 170});
        setBorderWidth(2.0f);
        setOpacity(0.72f);
    }

    void setOnClick(std::function<void()> cb) { m_onClick = std::move(cb); }

    void setLabel(const std::string& text) {
        if (m_labelPtr) m_labelPtr->setText(text);
    }

    bool onPointerEnter(const PointerEvent& event) override {
        (void)event;
        if (!m_pressed) setBorderColor(Color{255, 255, 255, 205});
        return true;
    }

    bool onPointerLeave(const PointerEvent& event) override {
        (void)event;
        m_pressed = false;
        setBorderColor(Color{255, 255, 255, 170});
        return true;
    }

    bool onPointerDown(const PointerEvent& event) override {
        (void)event;
        m_pressed = true;
        setBorderColor(Color{255, 255, 255, 235});
        return true;
    }

    bool onPointerUp(const PointerEvent& event) override {
        (void)event;
        bool wasPressed = m_pressed;
        m_pressed = false;
        setBorderColor(Color{255, 255, 255, 205});
        if (wasPressed && m_onClick) m_onClick();
        return true;
    }

private:
    std::unique_ptr<Text> m_label;
    Text* m_labelPtr{nullptr};
    std::function<void()> m_onClick;
    bool m_pressed{false};
};

} // namespace

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
    rootContainer->setBackgroundColor(Color{0, 0, 0, 0});

    // Inner Card Container
    auto cardContainer = std::make_unique<Container>();
    cardContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
    cardContainer->getYogaNode().setAlignItems(YGAlignCenter);
    cardContainer->getYogaNode().setPadding(YGEdgeAll, 24.0f);
    cardContainer->getYogaNode().setGap(YGGutterAll, 16.0f);
    cardContainer->setBackgroundColor(Color{255, 255, 255, 0});
    cardContainer->setBorderColor(Color{180, 210, 255, 0});
    cardContainer->setBorderWidth(0.0f);

    // Button & Text Widgets
    auto clickButton = std::make_unique<BlurButton>("Tıkla: 0");
    BlurButton* btnPtr = clickButton.get();
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
