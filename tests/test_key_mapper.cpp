#include <gtest/gtest.h>
#include "core/input/key_mapper.hpp"
#include <linux/input-event-codes.h>

using namespace lcl::core;

TEST(KeyMapperTest, LowercaseLetterMapping) {
    uint8_t noMods = 0;
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_A, noMods), 'a');
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_Z, noMods), 'z');
    EXPECT_EQ(KeyMapper::toUTF8(KEY_A, noMods), "a");
}

TEST(KeyMapperTest, ShiftUppercaseMapping) {
    uint8_t shiftMod = LCL_MOD_SHIFT;
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_A, shiftMod), 'A');
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_Z, shiftMod), 'Z');
    EXPECT_EQ(KeyMapper::toUTF8(KEY_A, shiftMod), "A");
}

TEST(KeyMapperTest, CapsLockToggleMapping) {
    uint8_t capsMod = LCL_MOD_CAPSLOCK;
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_A, capsMod), 'A');
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_Z, capsMod), 'Z');
    EXPECT_EQ(KeyMapper::toUTF8(KEY_A, capsMod), "A");
}

TEST(KeyMapperTest, ShiftAndCapsLockXOR) {
    uint8_t shiftAndCaps = LCL_MOD_SHIFT | LCL_MOD_CAPSLOCK;
    // Shift + CapsLock cancels out uppercase -> lowercase 'a'
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_A, shiftAndCaps), 'a');
    EXPECT_EQ(KeyMapper::toUTF8(KEY_A, shiftAndCaps), "a");
}

TEST(KeyMapperTest, ControlCharacterTranslation) {
    uint8_t ctrlMod = LCL_MOD_CTRL;
    // Ctrl+A -> \x01, Ctrl+C -> \x03
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_A, ctrlMod), 1);
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_C, ctrlMod), 3);
}

TEST(KeyMapperTest, NumbersAndSymbolsShiftMapping) {
    uint8_t noMods = 0;
    uint8_t shiftMod = LCL_MOD_SHIFT;

    EXPECT_EQ(KeyMapper::toCodepoint(KEY_1, noMods), '1');
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_1, shiftMod), '!');

    EXPECT_EQ(KeyMapper::toCodepoint(KEY_EQUAL, noMods), '=');
    EXPECT_EQ(KeyMapper::toCodepoint(KEY_EQUAL, shiftMod), '+');
}
