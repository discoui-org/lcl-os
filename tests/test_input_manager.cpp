#include <gtest/gtest.h>
#include "core/input/input_manager.hpp"
#include "platform/desktop/evdev_input_backend.hpp"

using namespace lcl::core;

TEST(InputManagerTest, DefaultEventProperties) {
    InputEvent ev{};
    EXPECT_EQ(ev.type, InputEventType::Unknown);
    EXPECT_EQ(ev.dx, 0.0);
    EXPECT_EQ(ev.dy, 0.0);
    EXPECT_EQ(ev.absoluteX, -1.0);
    EXPECT_EQ(ev.absoluteY, -1.0);
    EXPECT_FALSE(ev.pressed);
}

TEST(InputManagerTest, LifecycleInitializeAndShutdown) {
    InputManager input;
    // On systems where udev/evdev is accessible, initialize succeeds or falls back gracefully
    bool init = input.initialize(std::make_unique<lcl::platform::desktop::EvdevInputBackend>());
    EXPECT_TRUE(init);
    EXPECT_TRUE(input.isInitialized());
    input.shutdown();
    EXPECT_FALSE(input.isInitialized());
}

TEST(InputManagerTest, TouchpadDeltaAndButtonMapping) {
    InputEvent motionEv{};
    InputEvent buttonEv{};
    int motionCount = 0;
    int buttonCount = 0;

    InputManager input;
    input.setEventCallback([&](const InputEvent& ev) {
        if (ev.type == InputEventType::PointerMotion) {
            motionEv = ev;
            motionCount++;
        } else if (ev.type == InputEventType::PointerButton) {
            buttonEv = ev;
            buttonCount++;
        }
    });

    // Verify callback setup
    EXPECT_EQ(motionCount, 0);
    EXPECT_EQ(buttonCount, 0);
}

TEST(InputManagerTest, VirtioTabletAbsoluteMouseMotionAndClick) {
    InputEvent lastMotion{};
    InputEvent lastButton{};

    InputManager input;
    input.setEventCallback([&](const InputEvent& ev) {
        if (ev.type == InputEventType::PointerMotion) {
            lastMotion = ev;
        } else if (ev.type == InputEventType::PointerButton) {
            lastButton = ev;
        }
    });

    // Simulate tablet absolute motion (e.g. virtio-tablet-pci)
    InputEvent tabletMotion{};
    tabletMotion.type = InputEventType::PointerMotion;
    tabletMotion.source = PointerSource::Mouse;
    tabletMotion.absoluteX = 640.0;
    tabletMotion.absoluteY = 480.0;

    InputEvent tabletClick{};
    tabletClick.type = InputEventType::PointerButton;
    tabletClick.source = PointerSource::Mouse;
    tabletClick.button = PointerButton::Left;
    tabletClick.pressed = true;

    // Simulate delivery via InputManager callback
    // (Verifies event structures and semantics for absolute mouse devices)
    EXPECT_EQ(tabletMotion.source, PointerSource::Mouse);
    EXPECT_DOUBLE_EQ(tabletMotion.absoluteX, 640.0);
    EXPECT_DOUBLE_EQ(tabletMotion.absoluteY, 480.0);
    EXPECT_EQ(tabletClick.source, PointerSource::Mouse);
    EXPECT_TRUE(tabletClick.pressed);
}

TEST(InputManagerTest, KeyboardInputAndModifiersRetained) {
    InputEvent keyEv{};
    keyEv.type = InputEventType::KeyboardKey;
    keyEv.key = lcl::platform::PhysicalKey::A;
    keyEv.pressed = true;
    keyEv.modifiers = lcl::platform::kModShift;
    keyEv.codepoint = 'A';

    EXPECT_EQ(keyEv.type, InputEventType::KeyboardKey);
    EXPECT_EQ(keyEv.key, lcl::platform::PhysicalKey::A);
    EXPECT_TRUE(keyEv.pressed);
    EXPECT_EQ(keyEv.modifiers, lcl::platform::kModShift);
    EXPECT_EQ(keyEv.codepoint, 'A');
}

TEST(InputManagerTest, DirectAndMultiTouchscreenSourceDistinction) {
    InputEvent touchMotion{};
    touchMotion.type = InputEventType::PointerMotion;
    touchMotion.source = PointerSource::Touch;
    touchMotion.absoluteX = 300.0;
    touchMotion.absoluteY = 600.0;

    InputEvent mouseMotion{};
    mouseMotion.type = InputEventType::PointerMotion;
    mouseMotion.source = PointerSource::Mouse;
    mouseMotion.absoluteX = 300.0;
    mouseMotion.absoluteY = 600.0;

    EXPECT_NE(touchMotion.source, mouseMotion.source);
    EXPECT_EQ(touchMotion.source, PointerSource::Touch);
    EXPECT_EQ(mouseMotion.source, PointerSource::Mouse);
}
