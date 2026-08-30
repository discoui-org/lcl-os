#include <gtest/gtest.h>
#include "lcl-ui/core/events.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/touch_interaction.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/focus_scope.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/text_field.hpp"
#include "system/ipc/lcl_protocol.hpp"
#include "system/render/raster_canvas.hpp"
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

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

class FocusProbe : public Widget {
public:
    FocusProbe() { setFocusable(true); }

    bool onFocusGained(const FocusEvent& event) override {
        (void)event;
        ++focusGainedCount;
        return false;
    }

    bool onFocusLost(const FocusEvent& event) override {
        (void)event;
        ++focusLostCount;
        return false;
    }

    bool onKeyDown(const KeyEvent& event) override {
        if (event.key == lcl::platform::PhysicalKey::Tab) ++tabKeyDownCount;
        return true;
    }

    int focusGainedCount{0};
    int focusLostCount{0};
    int tabKeyDownCount{0};
};

bool dispatchTab(EventDispatcher& dispatcher, Widget* root,
                 bool backwards = false,
                 KeyEventType type = KeyEventType::KeyDown) {
    return dispatcher.dispatchKeyEvent(
        root,
        KeyEvent{lcl::platform::PhysicalKey::Tab,
                 static_cast<int>(lcl::platform::PhysicalKey::Tab), 0,
                 static_cast<uint8_t>(
                     backwards ? lcl::platform::kModShift : 0), type});
}

class TextFieldFocusTree {
public:
    TextFieldFocusTree() {
        root = std::make_unique<Container>();
        root->setWidth(240.0f);
        root->setHeight(80.0f);

        auto textField = std::make_unique<TextField>();
        field = textField.get();
        textField->setWidth(200.0f);
        root->addChild(std::move(textField));
        root->calculateLayout(240.0f, 80.0f);
        root->syncLayout();
    }

    bool pointer(PointerEventType type, PointerSource source, uint32_t pointerId) {
        return dispatcher.dispatchPointerEvent(root.get(),
            PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, type, source, pointerId});
    }

    EventDispatcher dispatcher;
    std::unique_ptr<Container> root;
    TextField* field{nullptr};
};

class FocusTransferTree {
public:
    FocusTransferTree() {
        root = std::make_unique<Container>();
        root->setDirection(layout::Direction::Column);
        root->setWidth(240.0f);
        root->setHeight(180.0f);

        auto firstField = std::make_unique<TextField>("A");
        first = firstField.get();
        firstField->setWidth(200.0f);
        firstField->setHeight(36.0f);

        auto secondField = std::make_unique<TextField>("B");
        second = secondField.get();
        secondField->setWidth(200.0f);
        secondField->setHeight(36.0f);

        auto nonFocusableChild = std::make_unique<Container>();
        nonFocusableChild->setWidth(200.0f);
        nonFocusableChild->setHeight(36.0f);

        root->addChild(std::move(firstField));
        root->addChild(std::move(secondField));
        root->addChild(std::move(nonFocusableChild));
        root->calculateLayout(240.0f, 180.0f);
        root->syncLayout();
    }

    bool pointer(PointerEventType type, PointerSource source, uint32_t pointerId,
                 float x, float y) {
        return dispatcher.dispatchPointerEvent(root.get(),
            PointerEvent{x, y, 0, 0.0f, 0.0f, type, source, pointerId});
    }

    EventDispatcher dispatcher;
    std::unique_ptr<Container> root;
    TextField* first{nullptr};
    TextField* second{nullptr};
};

class TapCompletionTextField : public TextField {
public:
    using TextField::TextField;

    bool onPointerUp(const PointerEvent& event) override {
        sawTouchTapCompletion = event.isTouchTapCompletion();
        return TextField::onPointerUp(event);
    }

    bool sawTouchTapCompletion{false};
};

class PointerCaptureWidget : public Widget {
public:
    bool onPointerDown(const PointerEvent& event) override {
        downPointerIds.push_back(event.pointerId);
        captureSucceeded = event.capturePointer(*this);
        return true;
    }

    bool onPointerMove(const PointerEvent& event) override {
        movePointerIds.push_back(event.pointerId);
        return true;
    }

    bool onPointerUp(const PointerEvent& event) override {
        upPointerIds.push_back(event.pointerId);
        return true;
    }

    bool onPointerCancel(const PointerEvent& event) override {
        cancelPointerIds.push_back(event.pointerId);
        return true;
    }

    bool captureSucceeded{false};
    std::vector<uint32_t> downPointerIds;
    std::vector<uint32_t> movePointerIds;
    std::vector<uint32_t> upPointerIds;
    std::vector<uint32_t> cancelPointerIds;
};

class PointerCaptureTree {
public:
    PointerCaptureTree() {
        root = std::make_unique<Container>();
        root->setDirection(layout::Direction::Row);
        root->setWidth(200.0f);
        root->setHeight(100.0f);

        auto leftWidget = std::make_unique<PointerCaptureWidget>();
        left = leftWidget.get();
        leftWidget->setWidth(100.0f);
        leftWidget->setHeight(100.0f);

        auto rightWidget = std::make_unique<PointerCaptureWidget>();
        right = rightWidget.get();
        rightWidget->setWidth(100.0f);
        rightWidget->setHeight(100.0f);

        root->addChild(std::move(leftWidget));
        root->addChild(std::move(rightWidget));
        root->calculateLayout(200.0f, 100.0f);
        root->syncLayout(0.0f, 0.0f);
    }

    EventDispatcher dispatcher;
    std::unique_ptr<Container> root;
    PointerCaptureWidget* left{nullptr};
    PointerCaptureWidget* right{nullptr};
};

TEST(LclUiEventsTest, CapturedPointerMoveIgnoresHitTestTarget) {
    PointerCaptureTree tree;
    constexpr uint32_t pointerId = 17;

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, pointerId});
    ASSERT_TRUE(tree.left->captureSucceeded);
    ASSERT_TRUE(tree.dispatcher.hasPointerCapture(pointerId, tree.left));

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{150.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, pointerId});

    EXPECT_EQ(tree.left->movePointerIds, std::vector<uint32_t>{pointerId});
    EXPECT_TRUE(tree.right->movePointerIds.empty());
}

TEST(LclUiEventsTest, CapturedPointerUpRoutesToOwnerAndAutomaticallyReleases) {
    PointerCaptureTree tree;
    constexpr uint32_t pointerId = 23;

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Mouse, pointerId});
    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{150.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up,
                     PointerSource::Mouse, pointerId});

    EXPECT_EQ(tree.left->upPointerIds, std::vector<uint32_t>{pointerId});
    EXPECT_TRUE(tree.right->upPointerIds.empty());
    EXPECT_FALSE(tree.dispatcher.hasPointerCapture(pointerId));
}

TEST(LclUiEventsTest, ExplicitPointerReleaseRestoresHitTesting) {
    PointerCaptureTree tree;
    constexpr uint32_t pointerId = 31;

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, pointerId});
    ASSERT_TRUE(tree.dispatcher.releasePointerCapture(pointerId, tree.left));

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{150.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, pointerId});

    EXPECT_TRUE(tree.left->movePointerIds.empty());
    EXPECT_EQ(tree.right->movePointerIds, std::vector<uint32_t>{pointerId});
}

TEST(LclUiEventsTest, PointerCapturesAreIndependentPerPointerId) {
    PointerCaptureTree tree;
    constexpr uint32_t leftPointerId = 41;
    constexpr uint32_t rightPointerId = 42;

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, leftPointerId});
    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{150.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, rightPointerId});

    ASSERT_TRUE(tree.dispatcher.hasPointerCapture(leftPointerId, tree.left));
    ASSERT_TRUE(tree.dispatcher.hasPointerCapture(rightPointerId, tree.right));

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{150.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, leftPointerId});
    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, rightPointerId});

    EXPECT_EQ(tree.left->movePointerIds, std::vector<uint32_t>{leftPointerId});
    EXPECT_EQ(tree.right->movePointerIds, std::vector<uint32_t>{rightPointerId});
}

TEST(LclUiEventsTest, PointerCancelRoutesToOwnerAndClearsCapture) {
    PointerCaptureTree tree;
    constexpr uint32_t pointerId = 53;

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, pointerId});
    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{150.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Cancel,
                     PointerSource::Touch, pointerId});

    EXPECT_EQ(tree.left->cancelPointerIds, std::vector<uint32_t>{pointerId});
    EXPECT_TRUE(tree.right->cancelPointerIds.empty());
    EXPECT_FALSE(tree.dispatcher.hasPointerCapture(pointerId));
}

TEST(LclUiEventsTest, InvalidCaptureOwnerReceivesCancelWhenStillAlive) {
    PointerCaptureTree tree;
    constexpr uint32_t pointerId = 61;

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, pointerId});
    tree.left->setVisible(false);

    tree.dispatcher.dispatchPointerEvent(tree.root.get(),
        PointerEvent{150.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, pointerId});

    EXPECT_EQ(tree.left->cancelPointerIds, std::vector<uint32_t>{pointerId});
    EXPECT_EQ(tree.right->movePointerIds, std::vector<uint32_t>{pointerId});
    EXPECT_FALSE(tree.dispatcher.hasPointerCapture(pointerId));
}

class TouchPanButton : public Button {
public:
    using Button::Button;

    bool onPointerCancel(const PointerEvent& event) override {
        ++cancelCount;
        return Button::onPointerCancel(event);
    }

    int cancelCount{0};
};

class ScrollViewTouchPanTree {
public:
    ScrollViewTouchPanTree() {
        root = std::make_unique<ScrollView>();
        scrollView = root.get();
        root->setWidth(200.0f);
        root->setHeight(100.0f);

        auto content = std::make_unique<Container>();
        content->setWidth(200.0f);

        auto child = std::make_unique<TouchPanButton>("Touch target");
        button = child.get();
        child->setWidth(200.0f);
        child->setHeight(400.0f);
        child->setOnClick([this]() { ++clickCount; });
        content->addChild(std::move(child));
        root->setContent(std::move(content));

        root->calculateLayout(200.0f, 100.0f);
        root->syncLayout(0.0f, 0.0f);
    }

    void dispatch(PointerEventType type, float y, uint32_t pointerId = 0) {
        dispatcher.dispatchPointerEvent(
            root.get(),
            PointerEvent{40.0f, y, 0, 0.0f, 0.0f, type,
                         PointerSource::Touch, pointerId});
    }

    EventDispatcher dispatcher;
    std::unique_ptr<ScrollView> root;
    ScrollView* scrollView{nullptr};
    TouchPanButton* button{nullptr};
    int clickCount{0};
};

TEST(LclUiEventsTest, ScrollViewSmallTouchMovePreservesChildClick) {
    ScrollViewTouchPanTree tree;

    tree.dispatch(PointerEventType::Down, 40.0f);
    tree.dispatch(PointerEventType::Move,
                  40.0f - ScrollView::kTouchDragThreshold * 0.5f);
    EXPECT_FALSE(tree.dispatcher.hasPointerCapture(0));
    EXPECT_FALSE(tree.scrollView->isTouchDragging());

    tree.dispatch(PointerEventType::Up,
                  40.0f - ScrollView::kTouchDragThreshold * 0.5f);

    EXPECT_EQ(tree.clickCount, 1);
    EXPECT_EQ(tree.button->cancelCount, 0);
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.button);
    EXPECT_FALSE(tree.scrollView->isTouchPanActive());
}

TEST(LclUiEventsTest, ScrollViewCapturesTouchAfterDragThreshold) {
    ScrollViewTouchPanTree tree;

    tree.dispatch(PointerEventType::Down, 40.0f);
    EXPECT_FALSE(tree.dispatcher.hasPointerCapture(0));
    tree.dispatch(PointerEventType::Move,
                  40.0f - ScrollView::kTouchDragThreshold);

    EXPECT_TRUE(tree.scrollView->isTouchDragging());
    EXPECT_TRUE(tree.dispatcher.hasPointerCapture(0, tree.scrollView));
}

TEST(LclUiEventsTest, ScrollViewTouchDragTracksFingerInScrollDirection) {
    ScrollViewTouchPanTree tree;
    tree.scrollView->setScrollY(80.0f);

    tree.dispatch(PointerEventType::Down, 50.0f);
    tree.dispatch(PointerEventType::Move, 30.0f);

    EXPECT_FLOAT_EQ(tree.scrollView->getScrollY(), 100.0f);
}

TEST(LclUiEventsTest, ScrollViewDragCancelsPressedChildAndSuppressesClick) {
    ScrollViewTouchPanTree tree;

    tree.dispatch(PointerEventType::Down, 40.0f);
    tree.dispatch(PointerEventType::Move, 20.0f);
    ASSERT_EQ(tree.button->cancelCount, 1);

    tree.dispatch(PointerEventType::Up, 20.0f);

    EXPECT_EQ(tree.clickCount, 0);
    EXPECT_EQ(tree.button->getState(), ButtonState::Normal);
}

TEST(LclUiEventsTest, ScrollViewPointerUpClearsGestureAndCapture) {
    ScrollViewTouchPanTree tree;

    tree.dispatch(PointerEventType::Down, 40.0f);
    tree.dispatch(PointerEventType::Move, 20.0f);
    ASSERT_TRUE(tree.scrollView->isTouchPanActive());
    ASSERT_TRUE(tree.dispatcher.hasPointerCapture(0, tree.scrollView));

    tree.dispatch(PointerEventType::Up, 20.0f);

    EXPECT_FALSE(tree.scrollView->isTouchPanActive());
    EXPECT_FALSE(tree.dispatcher.hasPointerCapture(0));
}

TEST(LclUiEventsTest, ScrollViewPointerCancelClearsGestureAndCapture) {
    ScrollViewTouchPanTree tree;

    tree.dispatch(PointerEventType::Down, 40.0f);
    tree.dispatch(PointerEventType::Move, 20.0f);
    ASSERT_TRUE(tree.scrollView->isTouchPanActive());
    ASSERT_TRUE(tree.dispatcher.hasPointerCapture(0, tree.scrollView));

    tree.dispatch(PointerEventType::Cancel, 20.0f);

    EXPECT_FALSE(tree.scrollView->isTouchPanActive());
    EXPECT_FALSE(tree.dispatcher.hasPointerCapture(0));
}

TEST(LclUiEventsTest, ScrollViewTouchDragClampsAtContentBounds) {
    ScrollViewTouchPanTree tree;
    constexpr uint32_t firstPointer = 7;
    constexpr uint32_t secondPointer = 8;

    tree.scrollView->setScrollY(100.0f);
    tree.dispatch(PointerEventType::Down, 50.0f, firstPointer);
    tree.dispatch(PointerEventType::Move, -500.0f, firstPointer);
    EXPECT_FLOAT_EQ(tree.scrollView->getScrollY(), tree.scrollView->getMaxScrollY());
    tree.dispatch(PointerEventType::Up, -500.0f, firstPointer);

    tree.scrollView->setScrollY(100.0f);
    tree.dispatch(PointerEventType::Down, 50.0f, secondPointer);
    tree.dispatch(PointerEventType::Move, 500.0f, secondPointer);
    EXPECT_FLOAT_EQ(tree.scrollView->getScrollY(), 0.0f);
}

TEST(LclUiEventsTest, DepthFirstHitTesting) {
    EventDispatcher dispatcher;

    auto root = std::make_unique<Container>();
    root->setWidth(400.0f);
    root->setHeight(400.0f);

    auto childContainer = std::make_unique<Container>();
    childContainer->setPositionType(layout::PositionType::Absolute);
    childContainer->setPosition(layout::Edge::Left, 50.0f);
    childContainer->setPosition(layout::Edge::Top, 50.0f);
    childContainer->setWidth(200.0f);
    childContainer->setHeight(200.0f);

    auto leafBtn = std::make_unique<Button>("Target");
    Button* leafPtr = leafBtn.get();
    leafBtn->setWidth(100.0f);
    leafBtn->setHeight(40.0f);

    childContainer->addChild(std::move(leafBtn));
    root->addChild(std::move(childContainer));

    root->calculateLayout(400.0f, 400.0f);
    root->syncLayout(0.0f, 0.0f);

    const auto textBounds = leafPtr->getTextWidget()->getAbsoluteBounds();
    Widget* hitResult = dispatcher.hitTest(
        root.get(), textBounds.x + textBounds.width * 0.5f,
        textBounds.y + textBounds.height * 0.5f);
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
    root->setDirection(layout::Direction::Row);
    root->setWidth(400.0f);
    root->setHeight(200.0f);

    auto w1 = std::make_unique<TestWidget>();
    TestWidget* w1Ptr = w1.get();
    w1->setWidth(100.0f);
    w1->setHeight(100.0f);

    auto w2 = std::make_unique<TestWidget>();
    TestWidget* w2Ptr = w2.get();
    w2->setWidth(100.0f);
    w2->setHeight(100.0f);

    root->addChild(std::move(w1));
    root->addChild(std::move(w2));

    root->calculateLayout(400.0f, 200.0f);
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
    root->setWidth(200.0f);
    root->setHeight(200.0f);

    auto btn = std::make_unique<Button>("Click Test");
    Button* btnPtr = btn.get();
    btn->setWidth(120.0f);
    btn->setHeight(40.0f);

    bool clicked = false;
    btn->setOnClick([&clicked]() {
        clicked = true;
    });

    root->addChild(std::move(btn));
    root->calculateLayout(200.0f, 200.0f);
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
    root->setWidth(200.0f);
    root->setHeight(200.0f);

    auto widget = std::make_unique<TestWidget>();
    TestWidget* widgetPtr = widget.get();
    widget->setWidth(100.0f);
    widget->setHeight(40.0f);

    root->addChild(std::move(widget));
    root->calculateLayout(200.0f, 200.0f);
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

TEST(LclUiEventsTest, KeyboardTraversalStartsAtScopeEndsWithoutFocus) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();

    auto first = std::make_unique<FocusProbe>();
    FocusProbe* firstPtr = first.get();
    auto nonFocusable = std::make_unique<Container>();
    auto last = std::make_unique<FocusProbe>();
    FocusProbe* lastPtr = last.get();
    root->addChild(std::move(first));
    root->addChild(std::move(nonFocusable));
    root->addChild(std::move(last));

    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), firstPtr);
    EXPECT_EQ(firstPtr->focusGainedCount, 1);

    dispatcher.setFocus(nullptr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get(), true));
    EXPECT_EQ(dispatcher.getFocusedWidget(), lastPtr);
    EXPECT_EQ(lastPtr->focusGainedCount, 1);
}

TEST(LclUiEventsTest, KeyboardTraversalUsesDepthFirstInsertionOrderAndWraps) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();

    auto first = std::make_unique<FocusProbe>();
    FocusProbe* firstPtr = first.get();
    auto group = std::make_unique<Container>();
    auto nestedFirst = std::make_unique<FocusProbe>();
    FocusProbe* nestedFirstPtr = nestedFirst.get();
    auto nestedSecond = std::make_unique<FocusProbe>();
    FocusProbe* nestedSecondPtr = nestedSecond.get();
    group->addChild(std::move(nestedFirst));
    group->addChild(std::move(nestedSecond));
    auto last = std::make_unique<FocusProbe>();
    FocusProbe* lastPtr = last.get();
    root->addChild(std::move(first));
    root->addChild(std::move(group));
    root->addChild(std::move(last));

    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), firstPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nestedFirstPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nestedSecondPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), lastPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), firstPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get(), true));
    EXPECT_EQ(dispatcher.getFocusedWidget(), lastPtr);

    dispatcher.setFocus(nestedSecondPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get(), true));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nestedFirstPtr);
}

TEST(LclUiEventsTest, NormalContainerDoesNotCreateATraversalTrap) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();
    auto before = std::make_unique<FocusProbe>();
    FocusProbe* beforePtr = before.get();
    auto formGroup = std::make_unique<Container>();
    auto field = std::make_unique<FocusProbe>();
    FocusProbe* fieldPtr = field.get();
    formGroup->addChild(std::move(field));
    auto after = std::make_unique<FocusProbe>();
    FocusProbe* afterPtr = after.get();
    root->addChild(std::move(before));
    root->addChild(std::move(formGroup));
    root->addChild(std::move(after));

    dispatcher.setFocus(beforePtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), fieldPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), afterPtr);
}

TEST(LclUiEventsTest, MoveFocusSelectsFirstEligibleWithoutSynthesizingFocus) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();
    auto disabled = std::make_unique<FocusProbe>();
    disabled->setInteractionEnabled(false);
    auto first = std::make_unique<FocusProbe>();
    FocusProbe* firstPtr = first.get();
    root->addChild(std::move(disabled));
    root->addChild(std::move(first));

    EXPECT_TRUE(dispatcher.moveFocus(root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), firstPtr);

    firstPtr->setInteractionEnabled(false);
    EXPECT_FALSE(dispatcher.moveFocus(root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nullptr);
}

TEST(LclUiEventsTest, KeyboardTraversalSkipsDisabledAndInputDisabledWidgets) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();

    auto nonFocusable = std::make_unique<Container>();
    auto disabledButton = std::make_unique<Button>("Disabled");
    Button* disabledButtonPtr = disabledButton.get();
    disabledButton->setEnabled(false);
    auto inputDisabled = std::make_unique<FocusProbe>();
    FocusProbe* inputDisabledPtr = inputDisabled.get();
    inputDisabled->setInteractionEnabled(false);
    auto active = std::make_unique<FocusProbe>();
    FocusProbe* activePtr = active.get();
    auto tail = std::make_unique<FocusProbe>();
    FocusProbe* tailPtr = tail.get();
    root->addChild(std::move(nonFocusable));
    root->addChild(std::move(disabledButton));
    root->addChild(std::move(inputDisabled));
    root->addChild(std::move(active));
    root->addChild(std::move(tail));

    EXPECT_FALSE(disabledButtonPtr->isInteractionEnabled());
    EXPECT_FALSE(inputDisabledPtr->isInteractionEnabled());
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), activePtr);

    activePtr->setInteractionEnabled(false);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), tailPtr);
    EXPECT_EQ(activePtr->focusLostCount, 1);
}

TEST(LclUiEventsTest, FocusScopeTrapsForwardAndBackwardTraversal) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();

    auto before = std::make_unique<FocusProbe>();
    FocusProbe* beforePtr = before.get();
    auto scope = std::make_unique<FocusScope>();
    auto first = std::make_unique<FocusProbe>();
    FocusProbe* firstPtr = first.get();
    auto last = std::make_unique<FocusProbe>();
    FocusProbe* lastPtr = last.get();
    scope->addChild(std::move(first));
    scope->addChild(std::move(last));
    auto after = std::make_unique<FocusProbe>();
    FocusProbe* afterPtr = after.get();
    root->addChild(std::move(before));
    root->addChild(std::move(scope));
    root->addChild(std::move(after));

    dispatcher.setFocus(beforePtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), firstPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), lastPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), firstPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get(), true));
    EXPECT_EQ(dispatcher.getFocusedWidget(), lastPtr);
    EXPECT_NE(dispatcher.getFocusedWidget(), afterPtr);
}

TEST(LclUiEventsTest, NestedFocusScopeUsesNearestFocusedAncestor) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();
    auto outer = std::make_unique<FocusScope>();

    auto outerFirst = std::make_unique<FocusProbe>();
    FocusProbe* outerFirstPtr = outerFirst.get();
    auto inner = std::make_unique<FocusScope>();
    auto innerFirst = std::make_unique<FocusProbe>();
    FocusProbe* innerFirstPtr = innerFirst.get();
    auto innerLast = std::make_unique<FocusProbe>();
    FocusProbe* innerLastPtr = innerLast.get();
    inner->addChild(std::move(innerFirst));
    inner->addChild(std::move(innerLast));
    auto outerLast = std::make_unique<FocusProbe>();
    FocusProbe* outerLastPtr = outerLast.get();
    outer->addChild(std::move(outerFirst));
    outer->addChild(std::move(inner));
    outer->addChild(std::move(outerLast));
    root->addChild(std::move(outer));

    dispatcher.setFocus(outerFirstPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), innerFirstPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), innerLastPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), innerFirstPtr);
    EXPECT_NE(dispatcher.getFocusedWidget(), outerLastPtr);
}

TEST(LclUiEventsTest, DestroyedFocusedWidgetAndScopeLeaveNoStaleFocus) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();
    auto fallback = std::make_unique<FocusProbe>();
    FocusProbe* fallbackPtr = fallback.get();
    auto scope = std::make_unique<FocusScope>();
    FocusScope* scopePtr = scope.get();
    auto focused = std::make_unique<FocusProbe>();
    FocusProbe* focusedPtr = focused.get();
    scope->addChild(std::move(focused));
    root->addChild(std::move(fallback));
    root->addChild(std::move(scope));

    dispatcher.setFocus(focusedPtr);
    ASSERT_EQ(dispatcher.getFocusedWidget(), focusedPtr);
    root->removeChild(scopePtr);
    EXPECT_EQ(dispatcher.getFocusedWidget(), nullptr);

    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), fallbackPtr);
}

TEST(LclUiEventsTest, TabIsConsumedBeforeFocusedTextFieldKeyRouting) {
    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();
    auto field = std::make_unique<TextField>("unchanged");
    TextField* fieldPtr = field.get();
    auto next = std::make_unique<FocusProbe>();
    FocusProbe* nextPtr = next.get();
    root->addChild(std::move(field));
    root->addChild(std::move(next));

    dispatcher.setFocus(fieldPtr);
    EXPECT_TRUE(dispatchTab(dispatcher, root.get()));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nextPtr);
    EXPECT_EQ(fieldPtr->getText(), "unchanged");
    EXPECT_EQ(nextPtr->tabKeyDownCount, 0);

    EXPECT_TRUE(dispatchTab(dispatcher, root.get(), false, KeyEventType::KeyUp));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nextPtr);
    EXPECT_EQ(nextPtr->tabKeyDownCount, 0);
}

TEST(LclUiEventsTest, WindowAppOwnsItsRootTraversalContext) {
    WindowApp app(lcl::render::makeRasterCanvas(), 200, 100,
                  "Focus traversal owner");
    auto root = std::make_unique<Container>();
    auto first = std::make_unique<FocusProbe>();
    FocusProbe* firstPtr = first.get();
    auto second = std::make_unique<FocusProbe>();
    FocusProbe* secondPtr = second.get();
    root->addChild(std::move(first));
    root->addChild(std::move(second));
    app.setRootWidget(std::move(root));

    const int tabKey = static_cast<int>(lcl::platform::PhysicalKey::Tab);
    EXPECT_TRUE(app.sendKeyDown(tabKey));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), firstPtr);
    EXPECT_TRUE(app.sendKeyDown(tabKey));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), secondPtr);
}

TEST(LclUiEventsTest, TextFieldMouseDownTakesFocus) {
    TextFieldFocusTree tree;

    EXPECT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.field);
    EXPECT_TRUE(tree.dispatcher.dispatchTextInputEvent(TextInputEvent{"Mouse"}));
    EXPECT_EQ(tree.field->getText(), "Mouse");
}

TEST(LclUiEventsTest, TextFieldTouchFocusWaitsForMatchingPointerUp) {
    TextFieldFocusTree tree;
    constexpr uint32_t pointerId = 7;

    EXPECT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, pointerId));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), nullptr);

    EXPECT_TRUE(tree.pointer(PointerEventType::Up, PointerSource::Touch, pointerId));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.field);
}

TEST(LclUiEventsTest, TextFieldTouchCancelDoesNotTakeFocus) {
    TextFieldFocusTree tree;
    constexpr uint32_t pointerId = 11;

    EXPECT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, pointerId));
    EXPECT_TRUE(tree.pointer(PointerEventType::Cancel, PointerSource::Touch, pointerId));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), nullptr);

    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, pointerId));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), nullptr);
}

TEST(LclUiEventsTest, TextFieldTouchUpFromDifferentPointerDoesNotTakeFocus) {
    TextFieldFocusTree tree;

    EXPECT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 17));
    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 18));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), nullptr);
}

TEST(LclUiEventsTest, MouseFocusTransfersAndDismissesAtDispatcherLevel) {
    FocusTransferTree tree;

    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    EXPECT_FALSE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 140.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), nullptr);

    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.second);

    EXPECT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.second);
}

TEST(LclUiEventsTest, TouchFocusWaitsForCompletedTapAndDismissesBackground) {
    FocusTransferTree tree;
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    EXPECT_FALSE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 41, 10.0f, 140.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);
    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 41, 10.0f, 140.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), nullptr);
}

TEST(LclUiEventsTest, TouchTapTransfersFocusAndIgnoresPointerIdMismatch) {
    FocusTransferTree tree;
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 42, 10.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);
    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 43, 10.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    ASSERT_TRUE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 42, 10.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.second);
}

TEST(LclUiEventsTest, TouchTapSlopUsesAnInclusiveSharedThreshold) {
    using touch_interaction::exceedsSlop;
    constexpr float slop = touch_interaction::kTouchSlop;
    EXPECT_FALSE(exceedsSlop(slop - 0.01f, 0.0f));
    EXPECT_TRUE(exceedsSlop(slop, 0.0f));
    EXPECT_TRUE(exceedsSlop(slop + 0.01f, 0.0f));
    EXPECT_FLOAT_EQ(ScrollView::kTouchDragThreshold, slop);

    FocusTransferTree tree;
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 47, 10.0f, 46.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Move, PointerSource::Touch, 47,
                              12.0f, 46.0f));
    ASSERT_TRUE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 47,
                             12.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.second);

    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 48, 10.0f, 46.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Move, PointerSource::Touch, 48,
                              10.0f + slop - 0.01f, 46.0f));
    ASSERT_TRUE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 48,
                             10.0f + slop - 0.01f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.second);

    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 49, 10.0f, 46.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Move, PointerSource::Touch, 49,
                              10.0f + slop, 46.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 49,
                              10.0f + slop, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 50, 10.0f, 46.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Move, PointerSource::Touch, 50,
                              10.0f + slop + 0.01f, 46.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Move, PointerSource::Touch, 50,
                              10.0f, 46.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 50,
                              10.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);
}

TEST(LclUiEventsTest, TouchDragWithoutScrollDoesNotDismissOrCompleteTextFieldTap) {
    constexpr float beyondSlop = touch_interaction::kTouchSlop + 1.0f;
    FocusTransferTree tree;
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    EXPECT_FALSE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 51, 10.0f, 140.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Move, PointerSource::Touch, 51,
                              10.0f + beyondSlop, 140.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 51,
                              10.0f + beyondSlop, 140.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    EventDispatcher dispatcher;
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(80.0f);
    auto field = std::make_unique<TapCompletionTextField>();
    TapCompletionTextField* fieldPtr = field.get();
    field->setWidth(200.0f);
    root->addChild(std::move(field));
    root->calculateLayout(200.0f, 80.0f);
    root->syncLayout();
    dispatcher.setFocus(fieldPtr);

    ASSERT_TRUE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, 52}));
    ASSERT_TRUE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up,
                     PointerSource::Touch, 52}));
    EXPECT_TRUE(fieldPtr->sawTouchTapCompletion);

    fieldPtr->sawTouchTapCompletion = false;
    ASSERT_TRUE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, 54}));
    EXPECT_FALSE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{12.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, 54}));
    ASSERT_TRUE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{12.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up,
                     PointerSource::Touch, 54}));
    EXPECT_TRUE(fieldPtr->sawTouchTapCompletion);

    fieldPtr->sawTouchTapCompletion = false;
    ASSERT_TRUE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, 53}));
    EXPECT_FALSE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{10.0f + beyondSlop, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, 53}));
    EXPECT_FALSE(dispatcher.dispatchPointerEvent(root.get(),
        PointerEvent{10.0f + beyondSlop, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up,
                     PointerSource::Touch, 53}));
    EXPECT_FALSE(fieldPtr->sawTouchTapCompletion);
    EXPECT_EQ(dispatcher.getFocusedWidget(), fieldPtr);
}

TEST(LclUiEventsTest, TouchCancelAndNonFocusableTapUseGenericFocusSemantics) {
    FocusTransferTree tree;
    ASSERT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Mouse, 0, 10.0f, 10.0f));
    ASSERT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    EXPECT_TRUE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 44, 10.0f, 46.0f));
    EXPECT_TRUE(tree.pointer(PointerEventType::Cancel, PointerSource::Touch, 44, 10.0f, 46.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), tree.first);

    EXPECT_FALSE(tree.pointer(PointerEventType::Down, PointerSource::Touch, 45, 10.0f, 82.0f));
    EXPECT_FALSE(tree.pointer(PointerEventType::Up, PointerSource::Touch, 45, 10.0f, 82.0f));
    EXPECT_EQ(tree.dispatcher.getFocusedWidget(), nullptr);
}

TEST(LclUiEventsTest, ScrollViewDragCancelsPendingTextFieldTouchFocus) {
    EventDispatcher dispatcher;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setDirection(layout::Direction::Column);
    auto field = std::make_unique<TextField>();
    field->setWidth(200.0f);
    auto filler = std::make_unique<Container>();
    filler->setWidth(200.0f);
    filler->setHeight(300.0f);
    content->addChild(std::move(field));
    content->addChild(std::move(filler));
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();

    constexpr uint32_t pointerId = 23;
    EXPECT_TRUE(dispatcher.dispatchPointerEvent(scrollView.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Touch, pointerId}));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nullptr);

    EXPECT_TRUE(dispatcher.dispatchPointerEvent(scrollView.get(),
        PointerEvent{10.0f, 30.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                     PointerSource::Touch, pointerId}));
    EXPECT_TRUE(scrollView->isTouchDragging());
    EXPECT_EQ(dispatcher.getFocusedWidget(), nullptr);

    EXPECT_TRUE(dispatcher.dispatchPointerEvent(scrollView.get(),
        PointerEvent{10.0f, 30.0f, 0, 0.0f, 0.0f, PointerEventType::Up,
                     PointerSource::Touch, pointerId}));
    EXPECT_EQ(dispatcher.getFocusedWidget(), nullptr);
}

TEST(LclUiEventsTest, ScrollViewTouchDragPreservesExistingFocus) {
    EventDispatcher dispatcher;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setDirection(layout::Direction::Column);
    auto field = std::make_unique<TextField>();
    TextField* fieldPtr = field.get();
    field->setWidth(200.0f);
    auto filler = std::make_unique<Container>();
    filler->setWidth(200.0f);
    filler->setHeight(300.0f);
    content->addChild(std::move(field));
    content->addChild(std::move(filler));
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();

    ASSERT_TRUE(dispatcher.dispatchPointerEvent(scrollView.get(),
        PointerEvent{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
                     PointerSource::Mouse, 0}));
    ASSERT_EQ(dispatcher.getFocusedWidget(), fieldPtr);

    const auto drag = [&](uint32_t pointerId, float startY) {
        // A non-clickable child may leave Down unhandled; ScrollView still
        // observes it in preview and owns the sequence after drag slop.
        dispatcher.dispatchPointerEvent(scrollView.get(),
            PointerEvent{10.0f, startY, 0, 0.0f, 0.0f, PointerEventType::Down,
                         PointerSource::Touch, pointerId});
        ASSERT_TRUE(dispatcher.dispatchPointerEvent(scrollView.get(),
            PointerEvent{10.0f, startY + 20.0f, 0, 0.0f, 0.0f, PointerEventType::Move,
                         PointerSource::Touch, pointerId}));
        ASSERT_TRUE(dispatcher.dispatchPointerEvent(scrollView.get(),
            PointerEvent{10.0f, startY + 20.0f, 0, 0.0f, 0.0f, PointerEventType::Up,
                         PointerSource::Touch, pointerId}));
        EXPECT_EQ(dispatcher.getFocusedWidget(), fieldPtr);
    };

    drag(46, 10.0f);  // Starts on the focused TextField.
    drag(47, 60.0f);  // Starts on a non-focusable content child.
}

TEST(LclUiEventsTest, TextFieldSupportsSingleLineNavigationAndEditingKeys) {
    TextField field("abcd");
    const auto key = [&field](lcl::platform::PhysicalKey physical) {
        return field.onKeyDown(KeyEvent{physical, static_cast<int>(physical)});
    };

    EXPECT_TRUE(key(lcl::platform::PhysicalKey::End));
    EXPECT_TRUE(key(lcl::platform::PhysicalKey::ArrowLeft));
    EXPECT_TRUE(key(lcl::platform::PhysicalKey::ArrowLeft));
    EXPECT_TRUE(key(lcl::platform::PhysicalKey::Backspace));
    EXPECT_EQ(field.getText(), "acd");

    EXPECT_TRUE(key(lcl::platform::PhysicalKey::Delete));
    EXPECT_EQ(field.getText(), "ad");
    EXPECT_TRUE(key(lcl::platform::PhysicalKey::ArrowRight));
    EXPECT_TRUE(field.onTextInput(TextInputEvent{"X"}));
    EXPECT_EQ(field.getText(), "adX");

    EXPECT_TRUE(key(lcl::platform::PhysicalKey::Home));
    EXPECT_TRUE(field.onTextInput(TextInputEvent{"H"}));
    EXPECT_EQ(field.getText(), "HadX");
    EXPECT_TRUE(key(lcl::platform::PhysicalKey::End));
    EXPECT_TRUE(field.onTextInput(TextInputEvent{"E\n"}));
    EXPECT_EQ(field.getText(), "HadXE");

    TextField utf8Field("açb");
    ASSERT_TRUE(utf8Field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::ArrowLeft,
                                             static_cast<int>(lcl::platform::PhysicalKey::ArrowLeft)}));
    ASSERT_TRUE(utf8Field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Backspace,
                                             static_cast<int>(lcl::platform::PhysicalKey::Backspace)}));
    EXPECT_EQ(utf8Field.getText(), "ab");
}

TEST(LclUiEventsTest, TextFieldEditingPreservesUtf8CodepointBoundaries) {
    const auto key = [](TextField& field, lcl::platform::PhysicalKey physical) {
        return field.onKeyDown(KeyEvent{physical, static_cast<int>(physical)});
    };

    TextField backspaceField("açb");
    ASSERT_TRUE(key(backspaceField, lcl::platform::PhysicalKey::End));
    ASSERT_TRUE(key(backspaceField, lcl::platform::PhysicalKey::ArrowLeft));
    ASSERT_TRUE(key(backspaceField, lcl::platform::PhysicalKey::Backspace));
    EXPECT_EQ(backspaceField.getText(), "ab");
    ASSERT_TRUE(backspaceField.onTextInput(TextInputEvent{"ş"}));
    EXPECT_EQ(backspaceField.getText(), "aşb");

    TextField deleteField("açb");
    ASSERT_TRUE(key(deleteField, lcl::platform::PhysicalKey::Home));
    ASSERT_TRUE(key(deleteField, lcl::platform::PhysicalKey::ArrowRight));
    ASSERT_TRUE(key(deleteField, lcl::platform::PhysicalKey::Delete));
    EXPECT_EQ(deleteField.getText(), "ab");

    TextField insertionField("aç");
    ASSERT_TRUE(key(insertionField, lcl::platform::PhysicalKey::Home));
    ASSERT_TRUE(key(insertionField, lcl::platform::PhysicalKey::ArrowRight));
    ASSERT_TRUE(insertionField.onTextInput(TextInputEvent{"ş"}));
    ASSERT_TRUE(insertionField.onTextInput(TextInputEvent{"ğ"}));
    EXPECT_EQ(insertionField.getText(), "aşğç");
    ASSERT_TRUE(key(insertionField, lcl::platform::PhysicalKey::Backspace));
    EXPECT_EQ(insertionField.getText(), "aşç");
    ASSERT_TRUE(key(insertionField, lcl::platform::PhysicalKey::Delete));
    EXPECT_EQ(insertionField.getText(), "aş");
}

TEST(LclUiEventsTest, TextFieldSetTextClampsCaretAndOnChangeRequiresAValueChange) {
    TextField field("abcd");
    int changes = 0;
    std::string lastValue;
    field.setOnChange([&](const std::string& value) {
        ++changes;
        lastValue = value;
    });

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::End,
                                         static_cast<int>(lcl::platform::PhysicalKey::End)}));
    field.setText("q");
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(lastValue, "q");

    field.setText("q");
    EXPECT_EQ(changes, 1);
    ASSERT_TRUE(field.onTextInput(TextInputEvent{"X"}));
    EXPECT_EQ(field.getText(), "qX");
    EXPECT_EQ(changes, 2);

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                         static_cast<int>(lcl::platform::PhysicalKey::Home)}));
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Backspace,
                                         static_cast<int>(lcl::platform::PhysicalKey::Backspace)}));
    EXPECT_EQ(field.getText(), "qX");
    EXPECT_EQ(changes, 2);
}

TEST(LclUiEventsTest, WindowAppDirectEventCallbacks) {
    WindowApp app(lcl::render::makeRasterCanvas(), 400, 300, "Test Window");

    int rawKeyCount = 0;
    int lastRawKey = 0;
    app.setOnRawKeyEvent([&rawKeyCount, &lastRawKey](const KeyEvent& ev) {
        rawKeyCount++;
        lastRawKey = ev.keyCode;
        return true; // Intercept & consume
    });

    bool handled = app.sendKeyDown(65, 'A', 0);
    EXPECT_TRUE(handled);
    EXPECT_EQ(rawKeyCount, 1);
    EXPECT_EQ(lastRawKey, 65);

    int rawPointerCount = 0;
    app.setOnRawPointerEvent([&rawPointerCount](const PointerEvent& ev) {
        (void)ev;
        rawPointerCount++;
        return true; // Intercept & consume
    });

    bool ptrHandled = app.sendPointerDown(50.0f, 50.0f, 0);
    EXPECT_TRUE(ptrHandled);
    EXPECT_EQ(rawPointerCount, 1);
}

TEST(LclUiEventsTest, VisualOnlyWindowIgnoresCompositorPointerEvents) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
    ASSERT_NE(fcntl(sockets[1], F_SETFL, fcntl(sockets[1], F_GETFL) | O_NONBLOCK), -1);

    {
        WindowApp panel(lcl::render::makeRasterCanvas(), 320, 32, "Visual Panel");
        panel.setSurfaceId(9);
        panel.setInputEnabled(false);
        panel.setExternalIpcSocket(sockets[1]);

        int pointerEvents = 0;
        panel.setOnRawPointerEvent([&pointerEvents](const PointerEvent&) {
            ++pointerEvents;
            return true;
        });

        lcl::protocol::LCLHeader header{};
        header.opcode = lcl::protocol::LCLOpcode::InputEvent;
        header.payloadSize = sizeof(lcl::protocol::LCLMsgInputEvent);
        lcl::protocol::LCLMsgInputEvent input{};
        input.surfaceId = 9;
        input.type = 3;
        input.x = 50.0f;
        input.y = 10.0f;
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[0], header, &input));

        panel.tick();
        EXPECT_EQ(pointerEvents, 0);
    }

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiEventsTest, PointerEventSourcePropagation) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
    ASSERT_NE(fcntl(sockets[1], F_SETFL, fcntl(sockets[1], F_GETFL) | O_NONBLOCK), -1);

    {
        WindowApp app(lcl::render::makeRasterCanvas(), 400, 300, "Source Test");
        app.setSurfaceId(1);
        app.setInputEnabled(true);
        app.setExternalIpcSocket(sockets[1]);

        std::vector<PointerEvent> receivedEvents;
        app.setOnRawPointerEvent([&receivedEvents](const PointerEvent& ev) {
            receivedEvents.push_back(ev);
            return true;
        });

        // 1. Send Mouse PointerMotion
        lcl::protocol::LCLHeader header{};
        header.opcode = lcl::protocol::LCLOpcode::InputEvent;
        header.payloadSize = sizeof(lcl::protocol::LCLMsgInputEvent);

        lcl::protocol::LCLMsgInputEvent mouseMotion{};
        mouseMotion.surfaceId = 1;
        mouseMotion.type = 3; // PointerMotion
        mouseMotion.x = 20.0f;
        mouseMotion.y = 30.0f;
        mouseMotion.source = static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Mouse);
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[0], header, &mouseMotion));

        // 2. Send Touch PointerDown
        lcl::protocol::LCLMsgInputEvent touchDown{};
        touchDown.surfaceId = 1;
        touchDown.type = 4; // PointerButton
        touchDown.pressed = 1;
        touchDown.key = 0;
        touchDown.x = 50.0f;
        touchDown.y = 60.0f;
        touchDown.source = static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch);
        touchDown.pointerId = 27;
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[0], header, &touchDown));

        // 3. Send Touch PointerUp
        lcl::protocol::LCLMsgInputEvent touchUp{};
        touchUp.surfaceId = 1;
        touchUp.type = 4; // PointerButton
        touchUp.pressed = 0;
        touchUp.key = 0;
        touchUp.x = 50.0f;
        touchUp.y = 60.0f;
        touchUp.source = static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch);
        touchUp.pointerId = 27;
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[0], header, &touchUp));

        // 4. Send Mouse PointerScroll
        lcl::protocol::LCLMsgInputEvent mouseScroll{};
        mouseScroll.surfaceId = 1;
        mouseScroll.type = 6; // PointerScroll
        mouseScroll.x = 25.0f;
        mouseScroll.y = 35.0f;
        mouseScroll.deltaX = 0.0f;
        mouseScroll.deltaY = 1.0f;
        mouseScroll.source = static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Mouse);
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[0], header, &mouseScroll));

        // 5. Cancel a second touch stream.
        lcl::protocol::LCLMsgInputEvent touchCancel{};
        touchCancel.surfaceId = 1;
        touchCancel.type = static_cast<uint32_t>(
            lcl::protocol::LCLInputEventType::PointerCancel);
        touchCancel.x = 70.0f;
        touchCancel.y = 80.0f;
        touchCancel.source = static_cast<uint8_t>(
            lcl::protocol::LCLPointerSource::Touch);
        touchCancel.pointerId = 28;
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(sockets[0], header, &touchCancel));

        app.tick();

        ASSERT_EQ(receivedEvents.size(), 5u);
        EXPECT_EQ(receivedEvents[0].type, PointerEventType::Move);
        EXPECT_EQ(receivedEvents[0].source, PointerSource::Mouse);
        EXPECT_FLOAT_EQ(receivedEvents[0].x, 20.0f);
        EXPECT_FLOAT_EQ(receivedEvents[0].y, 30.0f);

        EXPECT_EQ(receivedEvents[1].type, PointerEventType::Down);
        EXPECT_EQ(receivedEvents[1].source, PointerSource::Touch);
        EXPECT_EQ(receivedEvents[1].pointerId, 27u);
        EXPECT_FLOAT_EQ(receivedEvents[1].x, 50.0f);
        EXPECT_FLOAT_EQ(receivedEvents[1].y, 60.0f);

        EXPECT_EQ(receivedEvents[2].type, PointerEventType::Up);
        EXPECT_EQ(receivedEvents[2].source, PointerSource::Touch);
        EXPECT_EQ(receivedEvents[2].pointerId, 27u);
        EXPECT_FLOAT_EQ(receivedEvents[2].x, 50.0f);
        EXPECT_FLOAT_EQ(receivedEvents[2].y, 60.0f);

        EXPECT_EQ(receivedEvents[3].type, PointerEventType::Scroll);
        EXPECT_EQ(receivedEvents[3].source, PointerSource::Mouse);
        EXPECT_FLOAT_EQ(receivedEvents[3].x, 25.0f);
        EXPECT_FLOAT_EQ(receivedEvents[3].y, 35.0f);
        EXPECT_FLOAT_EQ(receivedEvents[3].deltaY, 1.0f);

        EXPECT_EQ(receivedEvents[4].type, PointerEventType::Cancel);
        EXPECT_EQ(receivedEvents[4].source, PointerSource::Touch);
        EXPECT_EQ(receivedEvents[4].pointerId, 28u);
        EXPECT_FLOAT_EQ(receivedEvents[4].x, 70.0f);
        EXPECT_FLOAT_EQ(receivedEvents[4].y, 80.0f);
    }

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiEventsTest, TouchPointerUpClearsHoverState) {
    EventDispatcher dispatcher;

    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(200.0f);

    auto btn = std::make_unique<Button>("Touch Test");
    Button* btnPtr = btn.get();
    btn->setWidth(120.0f);
    btn->setHeight(40.0f);

    bool clicked = false;
    btn->setOnClick([&clicked]() {
        clicked = true;
    });

    root->addChild(std::move(btn));
    root->calculateLayout(200.0f, 200.0f);
    root->syncLayout(0.0f, 0.0f);

    // 1. Touch move over button
    PointerEvent moveEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Move, PointerSource::Touch};
    dispatcher.dispatchPointerEvent(root.get(), moveEv);
    EXPECT_EQ(dispatcher.getHoveredWidget(), btnPtr);
    EXPECT_EQ(btnPtr->getState(), ButtonState::Hover);

    // 2. Touch down
    PointerEvent downEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down, PointerSource::Touch};
    dispatcher.dispatchPointerEvent(root.get(), downEv);
    EXPECT_EQ(btnPtr->getState(), ButtonState::Active);

    // 3. Touch up (lift finger)
    PointerEvent upEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up, PointerSource::Touch};
    dispatcher.dispatchPointerEvent(root.get(), upEv);

    EXPECT_TRUE(clicked);
    // Touch up must clear hover state on widget and in dispatcher
    EXPECT_EQ(dispatcher.getHoveredWidget(), nullptr);
    EXPECT_EQ(btnPtr->getState(), ButtonState::Focused);
}
