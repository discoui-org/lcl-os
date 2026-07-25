#include "core/display/display_scale.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace lcl::core {
namespace {

float clampScale(float s) {
    if (!std::isfinite(s) || s < 0.5f) {
        return 1.0f;
    }
    if (s > 4.0f) {
        return 4.0f;
    }
    const float snaps[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f};
    for (float c : snaps) {
        if (std::fabs(s - c) < 0.08f) {
            return c;
        }
    }
    return std::round(s * 100.0f) / 100.0f;
}

float parseScaleToken(const std::string& token) {
    // lcl.scale=2 or lcl.scale=2.0
    const std::string key = "lcl.scale=";
    auto pos = token.find(key);
    if (pos == std::string::npos) {
        return -1.0f;
    }
    try {
        // Accept leading junk / quotes: lcl.scale=2.0
        std::string val = token.substr(pos + key.size());
        while (!val.empty() && (val.back() == '"' || val.back() == '\'' || val.back() == '\r')) {
            val.pop_back();
        }
        return clampScale(std::stof(val));
    } catch (...) {
        return -1.0f;
    }
}

} // namespace

float& DisplayScale::scaleRef() {
    static float s = 1.0f;
    return s;
}

bool& DisplayScale::readyRef() {
    static bool ready = false;
    return ready;
}

void DisplayScale::initialize() {
    float found = -1.0f;

    if (const char* env = std::getenv("LCL_SCALE")) {
        try {
            found = clampScale(std::stof(env));
        } catch (...) {
        }
    }

    std::string cmdlineStr;
    std::ifstream cmdline("/proc/cmdline");
    if (cmdline) {
        std::getline(cmdline, cmdlineStr);
        std::istringstream iss(cmdlineStr);
        std::string token;
        while (iss >> token) {
            float s = parseScaleToken(token);
            if (s > 0.0f) {
                found = s;
            }
        }
        // Fallback: substring search if tokenizer missed it
        if (found < 0.0f) {
            float s = parseScaleToken(cmdlineStr);
            if (s > 0.0f) {
                found = s;
            }
        }
    }

    if (found > 0.0f) {
        scaleRef() = found;
    } else {
        scaleRef() = 1.0f;
    }
    readyRef() = true;

    std::cout << "[LCL DisplayScale] UI scale factor: " << scaleRef()
              << " (1 logical unit = " << px(1) << " px)\n";
    if (!cmdlineStr.empty()) {
        std::cout << "[LCL DisplayScale] cmdline: " << cmdlineStr << "\n";
    } else {
        std::cout << "[LCL DisplayScale] WARNING: /proc/cmdline empty or unreadable\n";
    }
}

float DisplayScale::factor() {
    if (!readyRef()) {
        initialize();
    }
    return scaleRef();
}

int DisplayScale::px(int logical) {
    if (logical == 0) {
        return 0;
    }
    const float s = factor();
    if (s == 1.0f) {
        return logical;
    }
    int out = static_cast<int>(std::lround(static_cast<float>(logical) * s));
    if (logical > 0) {
        return std::max(1, out);
    }
    return std::min(-1, out);
}

float DisplayScale::pxF(float logical) {
    return logical * factor();
}

} // namespace lcl::core
