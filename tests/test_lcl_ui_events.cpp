#include <gtest/gtest.h>
#include "lcl-ui/core/events.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "render/skia_canvas.hpp"
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
        root->getYogaNode().setDirection(YGFlexDirectionRow);
        root->getYogaNode().setWidth(200.0f);
        root->getYogaNode().setHeight(100.0f);

        auto leftWidget = std::make_unique<PointerCaptureWidget>();
        left = leftWidget.get();
        leftWidget->getYogaNode().setWidth(100.0f);
        leftWidget->getYogaNode().setHeight(100.0f);

        auto rightWidget = std::make_unique<PointerCaptureWidget>();
        right = rightWidget.get();
        rightWidget->getYogaNode().setWidth(100.0f);
        rightWidget->getYogaNode().setHeight(100.0f);

        root->addChild(std::move(leftWidget));
        root->addChild(std::move(rightWidget));
        root->getYogaNode().calculateLayout(200.0f, 100.0f);
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

TEST(LclUiEventsTest, WindowAppDirectEventCallbacks) {
    WindowApp app(lcl::render::makeSkiaCanvas(), 400, 300, "Test Window");

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
        WindowApp panel(lcl::render::makeSkiaCanvas(), 320, 32, "Visual Panel");
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
        WindowApp app(lcl::render::makeSkiaCanvas(), 400, 300, "Source Test");
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

        app.tick();

        ASSERT_EQ(receivedEvents.size(), 4u);
        EXPECT_EQ(receivedEvents[0].type, PointerEventType::Move);
        EXPECT_EQ(receivedEvents[0].source, PointerSource::Mouse);
        EXPECT_FLOAT_EQ(receivedEvents[0].x, 20.0f);
        EXPECT_FLOAT_EQ(receivedEvents[0].y, 30.0f);

        EXPECT_EQ(receivedEvents[1].type, PointerEventType::Down);
        EXPECT_EQ(receivedEvents[1].source, PointerSource::Touch);
        EXPECT_FLOAT_EQ(receivedEvents[1].x, 50.0f);
        EXPECT_FLOAT_EQ(receivedEvents[1].y, 60.0f);

        EXPECT_EQ(receivedEvents[2].type, PointerEventType::Up);
        EXPECT_EQ(receivedEvents[2].source, PointerSource::Touch);
        EXPECT_FLOAT_EQ(receivedEvents[2].x, 50.0f);
        EXPECT_FLOAT_EQ(receivedEvents[2].y, 60.0f);

        EXPECT_EQ(receivedEvents[3].type, PointerEventType::Scroll);
        EXPECT_EQ(receivedEvents[3].source, PointerSource::Mouse);
        EXPECT_FLOAT_EQ(receivedEvents[3].x, 25.0f);
        EXPECT_FLOAT_EQ(receivedEvents[3].y, 35.0f);
        EXPECT_FLOAT_EQ(receivedEvents[3].deltaY, 1.0f);
    }

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiEventsTest, TouchPointerUpClearsHoverState) {
    EventDispatcher dispatcher;

    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(200.0f);
    root->getYogaNode().setHeight(200.0f);

    auto btn = std::make_unique<Button>("Touch Test");
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
    EXPECT_EQ(btnPtr->getState(), ButtonState::Normal);
}
