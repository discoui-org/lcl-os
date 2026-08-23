#include "platform/common/output_scale.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace lcl::platform {
namespace {

float parseScaleToken(const std::string& token) {
    constexpr const char* key = "lcl.scale=";
    const auto pos = token.find(key);
    if (pos == std::string::npos) {
        return -1.0f;
    }

    try {
        std::string value = token.substr(pos + std::char_traits<char>::length(key));
        while (!value.empty() &&
               (value.back() == '"' || value.back() == '\'' ||
                value.back() == '\r')) {
            value.pop_back();
        }
        return sanitizeOutputScale(std::stof(value));
    } catch (...) {
        return -1.0f;
    }
}

} // namespace

float sanitizeOutputScale(float scale) noexcept {
    if (!std::isfinite(scale) || scale < 0.5f) {
        return 1.0f;
    }
    if (scale > 4.0f) {
        return 4.0f;
    }

    constexpr float commonScales[]{
        1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f,
    };
    for (const float candidate : commonScales) {
        if (std::fabs(scale - candidate) < 0.08f) {
            return candidate;
        }
    }
    return std::round(scale * 100.0f) / 100.0f;
}

float resolveOutputScale() {
    float resolved = -1.0f;

    if (const char* environmentScale = std::getenv("LCL_SCALE")) {
        try {
            resolved = sanitizeOutputScale(std::stof(environmentScale));
        } catch (...) {
        }
    }

    std::ifstream cmdline("/proc/cmdline");
    if (cmdline) {
        std::string cmdlineText;
        std::getline(cmdline, cmdlineText);
        std::istringstream tokens(cmdlineText);
        std::string token;
        while (tokens >> token) {
            const float parsed = parseScaleToken(token);
            if (parsed > 0.0f) {
                resolved = parsed;
            }
        }
        if (resolved < 0.0f) {
            const float parsed = parseScaleToken(cmdlineText);
            if (parsed > 0.0f) {
                resolved = parsed;
            }
        }
    }

    return resolved > 0.0f ? resolved : 1.0f;
}

} // namespace lcl::platform
