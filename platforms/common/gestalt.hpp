#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lcl::platform {

enum class NaturalOrientation : uint8_t {
    Auto,
    Portrait,
    Landscape,
};

enum class ShellKind : uint8_t {
    Desktop,
    Mobile,
};

struct GestaltInsets {
    float top{0.0f};
    float right{0.0f};
    float bottom{0.0f};
    float left{0.0f};
};

struct GestaltCorner {
    float radiusX{0.0f};
    float radiusY{0.0f};
    float roundness{2.0f};
};

struct GestaltCorners {
    GestaltCorner topLeft;
    GestaltCorner topRight;
    GestaltCorner bottomLeft;
    GestaltCorner bottomRight;
};

struct GestaltCutout {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

struct DisplayGestalt {
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
    std::optional<uint32_t> refreshRateHz;
    std::optional<float> scale;
    NaturalOrientation naturalOrientation{NaturalOrientation::Auto};
    uint16_t defaultRotation{0};
    GestaltInsets safeArea;
    GestaltCorners corners;
    std::vector<GestaltCutout> cutouts;

    bool hasPreferredResolution() const noexcept {
        return width.has_value() && height.has_value();
    }
};

struct DeviceGestalt {
    uint32_t version{1};
    std::string name{"LCL Default"};
    ShellKind shell{ShellKind::Desktop};
    DisplayGestalt display;
};

struct GestaltLoadResult {
    DeviceGestalt gestalt;
    std::string path;
    std::string error;
    bool loadedFromFile{false};
    bool explicitOverride{false};

    bool ok() const noexcept { return error.empty(); }
};

/** Parse one complete, versioned Gestalt JSON document. */
bool parseGestaltJson(std::string_view json, DeviceGestalt& destination,
                      std::string& error);

/**
 * Load LCL_GESTALT_PATH when set, otherwise the platform default path.
 * A missing platform default yields the built-in rectangular 1x Gestalt. An
 * explicit override that cannot be loaded is an error.
 */
GestaltLoadResult loadGestalt(const std::string& platformDefaultPath);

} // namespace lcl::platform
