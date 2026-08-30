#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace lcl::render {

struct WindowRenderContent {
    uint32_t windowId{0};
    std::vector<std::string> lines;
    std::string suggestion;
    int cursorCol{-1};
    bool forceCursorSolid{false};
};

} // namespace lcl::render
