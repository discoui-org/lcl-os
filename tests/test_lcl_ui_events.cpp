#include <gtest/gtest.h>
#include "lcl-ui/core/events.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/text.hpp"

using namespace lcl::ui;

class TestWidget : public Widget {
public:
    TestWidget() {
        setFocusable(true);
    }

    bool onPointerEnter(const PointerEvent& event) override {
        (void)event;
        entered = true;
        left = false;
        return true;
    }

    bool onPointerLeave(const PointerEvent& event) override {
        (void)event;
        left = true;
        entered = false;
        return true;
    }

    bool onKeyDown(const KeyEvent& event) override {
        lastKeyCode = event.keyCode;
        return true;
    }

    bool onTextInput(const TextInputEvent& event) override {
        lastTextInput = event.text;
        return true;
    }

    bool entered{false};
    bool left{false};
    int lastKeyCode{-1};
    std::string lastTextInput;
};

TEST(LclUiEventsTest, DepthFirstHitTesting) {
    EventDispatcher dispatcher;

    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(400.0f);
    root->getYogaNode().setHeight(400.0f);

    auto childContainer = std::make_unique<Container>();
    childContainer->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    childContainer->getYogaNode().setPosition(YGEdgeLeft, 50.0f);
    childContainer->getYogaNode().setPosition(YGEdgeTop, 50.0f);
    childContainer->getYogaNode().setWidth(200.0f);
    childContainer->getYogaNode().setHeight(200.0f);

    auto leafBtn = std::make_unique<Button>("Target");
    Button* leafPtr = leafBtn.get();
    leafBtn->getYogaNode().setWidth(100.0f);
    leafBtn->getYogaNode().setHeight(40.0f);

    childContainer->addChild(std::move(leafBtn));
    root->addChild(std::move(childContainer));

    root->getYogaNode().calculateLayout(400.0f, 400.0f);
    root->syncLayout(0.0f, 0.0f);

    // Coordinate inside childContainer (50, 50) + leafBtn centered Text -> (80, 65)
    Widget* hitResult = dispatcher.hitTest(root.get(), 80.0f, 65.0f);
    EXPECT_EQ(hitResult, leafPtr->getTextWidget()); // Leaf text inside button

    // Coordinate inside leaf button padding area -> button itself
    Widget* btnHit = dispatcher.hitTest(root.get(), 55.0f, 55.0f);
    EXPECT_EQ(btnHit, leafPtr);

    // Coordinate outside leaf button but inside root -> root
    Widget* rootHit = dispatcher.hitTest(root.get(), 10.0f, 10.0f);
    EXPECT_EQ(rootHit, root.get());
}

TEST(LclUiEventsTest, HoverTransitionEvents) {
    EventDispatcher dispatcher;

    auto root = std::make_unique<Container>();
    root->getYogaNode().setDirection(YGFlexDirectionRow);
    root->getYogaNode().setWidth(400.0f);
    root->getYogaNode().setHeight(200.0f);

    auto w1 = std::make_unique<TestWidget>();
    TestWidget* w1Ptr = w1.get();
    w1->getYogaNode().setWidth(100.0f);
    w1->getYogaNode().setHeight(100.0f);

    auto w2 = std::make_unique<TestWidget>();
    TestWidget* w2Ptr = w2.get();
    w2->getYogaNode().setWidth(100.0f);
    w2->getYogaNode().setHeight(100.0f);

    root->addChild(std::move(w1));
    root->addChild(std::move(w2));

    root->getYogaNode().calculateLayout(400.0f, 200.0f);
    root->syncLayout(0.0f, 0.0f);

    // Move pointer over w1 (10, 10)
    PointerEvent ev1{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move};
    dispatcher.dispatchPointerEvent(root.get(), ev1);

    EXPECT_TRUE(w1Ptr->entered);
    EXPECT_FALSE(w1Ptr->left);
    EXPECT_EQ(dispatcher.getHoveredWidget(), w1Ptr);

    // Move pointer from w1 to w2 (110, 10)
    PointerEvent ev2{110.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move};
    dispatcher.dispatchPointerEvent(root.get(), ev2);

    EXPECT_TRUE(w1Ptr->left);
    EXPECT_TRUE(w2Ptr->entered);
    EXPECT_EQ(dispatcher.getHoveredWidget(), w2Ptr);
}

TEST(LclUiEventsTest, ButtonClickPipeline) {
    EventDispatcher dispatcher;

    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(200.0f);
    root->getYogaNode().setHeight(200.0f);

    auto btn = std::make_unique<Button>("Click Test");
    Button* btnPtr = btn.get();
    btn->getYogaNode().setWidth(120.0f);
    btn->getYogaNode().setHeight(40.0f);

    bool clicked = false;
    btn->setOnClick([&clicked]() {
        clicked = true;
    });

    root->addChild(std::move(btn));
    root->getYogaNode().calculateLayout(200.0f, 200.0f);
    root->syncLayout(0.0f, 0.0f);

    // Dispatch PointerDown over button (10, 10)
    PointerEvent downEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down};
    dispatcher.dispatchPointerEvent(root.get(), downEv);
    EXPECT_EQ(btnPtr->getState(), ButtonState::Active);

    // Dispatch PointerUp over button (10, 10)
    PointerEvent upEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up};
    dispatcher.dispatchPointerEvent(root.get(), upEv);

    EXPECT_TRUE(clicked);
    EXPECT_EQ(btnPtr->getState(), ButtonState::Hover);
}

TEST(LclUiEventsTest, KeyboardEventRouting) {
    EventDispatcher dispatcher;

    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(200.0f);
    root->getYogaNode().setHeight(200.0f);

    auto widget = std::make_unique<TestWidget>();
    TestWidget* widgetPtr = widget.get();
    widget->getYogaNode().setWidth(100.0f);
    widget->getYogaNode().setHeight(40.0f);

    root->addChild(std::move(widget));
    root->getYogaNode().calculateLayout(200.0f, 200.0f);
    root->syncLayout(0.0f, 0.0f);

    // Focus widget by clicking it
    PointerEvent downEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down};
    dispatcher.dispatchPointerEvent(root.get(), downEv);
    EXPECT_EQ(dispatcher.getFocusedWidget(), widgetPtr);

    // Dispatch KeyDown event (Key 42)
    KeyEvent keyEv{42, 0, 0, KeyEventType::KeyDown};
    bool keyHandled = dispatcher.dispatchKeyEvent(keyEv);
    EXPECT_TRUE(keyHandled);
    EXPECT_EQ(widgetPtr->lastKeyCode, 42);

    // Dispatch TextInput event ("Hello")
    TextInputEvent textEv{"Hello"};
    bool textHandled = dispatcher.dispatchTextInputEvent(textEv);
    EXPECT_TRUE(textHandled);
    EXPECT_EQ(widgetPtr->lastTextInput, "Hello");
}
