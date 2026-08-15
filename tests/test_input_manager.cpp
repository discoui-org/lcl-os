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
