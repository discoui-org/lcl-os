#include <gtest/gtest.h>
#include "platforms/common/keyboard_mapper.hpp"
#include "platforms/linux/evdev_key_mapper.hpp"
#include <linux/input-event-codes.h>

using namespace lcl::platform;

TEST(EvdevKeyMapperTest, LinuxKeycodeToPhysicalKey) {
    EXPECT_EQ(desktop::EvdevKeyMapper::toPhysicalKey(KEY_A), PhysicalKey::A);
    EXPECT_EQ(desktop::EvdevKeyMapper::toPhysicalKey(KEY_Z), PhysicalKey::Z);
    EXPECT_EQ(desktop::EvdevKeyMapper::toPhysicalKey(KEY_1), PhysicalKey::Digit1);
    EXPECT_EQ(desktop::EvdevKeyMapper::toPhysicalKey(KEY_0), PhysicalKey::Digit0);
    EXPECT_EQ(desktop::EvdevKeyMapper::toPhysicalKey(KEY_ENTER), PhysicalKey::Enter);
    EXPECT_EQ(desktop::EvdevKeyMapper::toPhysicalKey(KEY_LEFTSHIFT), PhysicalKey::LeftShift);
    EXPECT_EQ(desktop::EvdevKeyMapper::toPhysicalKey(KEY_UP), PhysicalKey::ArrowUp);
}

TEST(KeyboardMapperTest, LowercaseLetterMapping) {
    uint8_t noMods = 0;
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::A, noMods), 'a');
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::Z, noMods), 'z');
    EXPECT_EQ(KeyboardMapper::toUTF8(PhysicalKey::A, noMods), "a");
}

TEST(KeyboardMapperTest, ShiftUppercaseMapping) {
    uint8_t shiftMod = kModShift;
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::A, shiftMod), 'A');
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::Z, shiftMod), 'Z');
    EXPECT_EQ(KeyboardMapper::toUTF8(PhysicalKey::A, shiftMod), "A");
}

TEST(KeyboardMapperTest, CapsLockToggleMapping) {
    uint8_t capsMod = kModCapsLock;
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::A, capsMod), 'A');
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::Z, capsMod), 'Z');
    EXPECT_EQ(KeyboardMapper::toUTF8(PhysicalKey::A, capsMod), "A");
}

TEST(KeyboardMapperTest, ShiftAndCapsLockXOR) {
    uint8_t shiftAndCaps = kModShift | kModCapsLock;
    // Shift + CapsLock cancels out uppercase -> lowercase 'a'
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::A, shiftAndCaps), 'a');
    EXPECT_EQ(KeyboardMapper::toUTF8(PhysicalKey::A, shiftAndCaps), "a");
}

TEST(KeyboardMapperTest, ControlCharacterTranslation) {
    uint8_t ctrlMod = kModCtrl;
    // Ctrl+A -> \x01, Ctrl+C -> \x03
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::A, ctrlMod), 1);
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::C, ctrlMod), 3);
}

TEST(KeyboardMapperTest, NumbersAndSymbolsShiftMapping) {
    uint8_t noMods = 0;
    uint8_t shiftMod = kModShift;

    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::Digit1, noMods), '1');
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::Digit1, shiftMod), '!');

    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::Equal, noMods), '=');
    EXPECT_EQ(KeyboardMapper::toCodepoint(PhysicalKey::Equal, shiftMod), '+');
}

TEST(KeyboardMapperTest, ModifierDetection) {
    EXPECT_TRUE(KeyboardMapper::isModifier(PhysicalKey::LeftShift));
    EXPECT_TRUE(KeyboardMapper::isModifier(PhysicalKey::RightCtrl));
    EXPECT_TRUE(KeyboardMapper::isModifier(PhysicalKey::LeftAlt));
    EXPECT_TRUE(KeyboardMapper::isModifier(PhysicalKey::RightMeta));
    EXPECT_TRUE(KeyboardMapper::isModifier(PhysicalKey::CapsLock));
    EXPECT_FALSE(KeyboardMapper::isModifier(PhysicalKey::A));
    EXPECT_FALSE(KeyboardMapper::isModifier(PhysicalKey::Enter));
}
