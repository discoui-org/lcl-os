#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "lcl-graphics/display_list.hpp"

namespace lcl::graphics {

constexpr uint32_t kDisplayListWireMagic = 0x314C444Cu; // "LDL1"
constexpr uint16_t kDisplayListWireVersion = 2;
constexpr size_t kDefaultDisplayListWireLimit = 1024u * 1024u;

enum class DisplayListWireError : uint8_t {
    None = 0,
    UnsupportedCommand,
    InvalidData,
    TooLarge,
    UnsupportedVersion,
};

struct DisplayListEncodeResult {
    std::vector<uint8_t> bytes;
    DisplayListWireError error{DisplayListWireError::None};

    explicit operator bool() const noexcept {
        return error == DisplayListWireError::None;
    }
};

struct DisplayListDecodeResult {
    DisplayList displayList;
    DisplayListWireError error{DisplayListWireError::None};

    explicit operator bool() const noexcept {
        return error == DisplayListWireError::None;
    }
};

/** Encodes backend-neutral commands. Images carry only stable resource IDs. */
DisplayListEncodeResult encodeDisplayList(
    const DisplayList& displayList,
    size_t maxBytes = kDefaultDisplayListWireLimit);

/** Decodes a bounded, versioned display list received from an untrusted peer. */
DisplayListDecodeResult decodeDisplayList(
    std::span<const uint8_t> bytes,
    size_t maxBytes = kDefaultDisplayListWireLimit);

} // namespace lcl::graphics
