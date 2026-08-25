#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "platform/common/gestalt.hpp"

namespace lcl::platform {
namespace {

class ScopedGestaltEnvironment {
public:
    ScopedGestaltEnvironment() {
        if (const char* current = std::getenv("LCL_GESTALT_PATH")) m_previous = current;
        unsetenv("LCL_GESTALT_PATH");
    }
    ~ScopedGestaltEnvironment() {
        if (m_previous) setenv("LCL_GESTALT_PATH", m_previous->c_str(), 1);
        else unsetenv("LCL_GESTALT_PATH");
    }

private:
    std::optional<std::string> m_previous;
};

} // namespace

TEST(GestaltTest, ParsesVersionedDisplayGeometryAndPolicy) {
    constexpr std::string_view json = R"json({
        "version": 1,
        "name": "Test Tablet",
        "shell": "mobile",
        "display": {
            "width": 1440,
            "height": 2960,
            "refreshRateHz": 60,
            "scale": 3.0,
            "naturalOrientation": "portrait",
            "defaultRotation": 0,
            "safeArea": {"top": 48, "right": 0, "bottom": 24, "left": 0},
            "corners": {
                "topLeft": {"radiusX": 96, "radiusY": 92, "roundness": 2.5},
                "topRight": {"radiusX": 96, "radiusY": 92},
                "bottomLeft": {"radiusX": 80, "radiusY": 80},
                "bottomRight": {"radiusX": 80, "radiusY": 80}
            },
            "cutouts": [{"x": 600, "y": 0, "width": 240, "height": 64}]
        }
    })json";

    DeviceGestalt gestalt;
    std::string error;
    ASSERT_TRUE(parseGestaltJson(json, gestalt, error)) << error;
    EXPECT_EQ(gestalt.version, 1u);
    EXPECT_EQ(gestalt.name, "Test Tablet");
    EXPECT_EQ(gestalt.shell, ShellKind::Mobile);
    ASSERT_TRUE(gestalt.display.hasPreferredResolution());
    EXPECT_EQ(*gestalt.display.width, 1440u);
    EXPECT_EQ(*gestalt.display.height, 2960u);
    ASSERT_TRUE(gestalt.display.scale.has_value());
    EXPECT_FLOAT_EQ(*gestalt.display.scale, 3.0f);
    EXPECT_EQ(gestalt.display.naturalOrientation, NaturalOrientation::Portrait);
    EXPECT_FLOAT_EQ(gestalt.display.safeArea.top, 48.0f);
    EXPECT_FLOAT_EQ(gestalt.display.corners.topLeft.radiusX, 96.0f);
    EXPECT_FLOAT_EQ(gestalt.display.corners.topLeft.roundness, 2.5f);
    ASSERT_EQ(gestalt.display.cutouts.size(), 1u);
    EXPECT_FLOAT_EQ(gestalt.display.cutouts.front().width, 240.0f);
}

TEST(GestaltTest, ShellDefaultsToDesktopAndRejectsUnknownKinds) {
    DeviceGestalt gestalt;
    std::string error;
    ASSERT_TRUE(parseGestaltJson(
        R"json({"version":1,"display":{}})json", gestalt, error)) << error;
    EXPECT_EQ(gestalt.shell, ShellKind::Desktop);

    EXPECT_FALSE(parseGestaltJson(
        R"json({"version":1,"shell":"tablet","display":{}})json",
        gestalt, error));
    EXPECT_NE(error.find("desktop or mobile"), std::string::npos);
}

TEST(GestaltTest, RequiresResolutionDimensionsAsAPair) {
    DeviceGestalt gestalt;
    std::string error;
    EXPECT_FALSE(parseGestaltJson(
        R"json({"version":1,"display":{"width":1440}})json",
        gestalt, error));
    EXPECT_NE(error.find("specified together"), std::string::npos);
}

TEST(GestaltTest, RejectsUnknownKeysAndTrailingJson) {
    DeviceGestalt gestalt;
    std::string error;
    EXPECT_FALSE(parseGestaltJson(
        R"json({"version":1,"display":{"androidRadius":42}})json",
        gestalt, error));
    EXPECT_NE(error.find("unknown key"), std::string::npos);

    EXPECT_FALSE(parseGestaltJson(
        R"json({"version":1,"display":{}} {})json", gestalt, error));
    EXPECT_NE(error.find("trailing content"), std::string::npos);
}

TEST(GestaltTest, MissingPlatformFileUsesBuiltInRectangularDefault) {
    ScopedGestaltEnvironment environment;
    const auto result = loadGestalt("/definitely/not/a/real/lcl-gestalt.json");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_FALSE(result.loadedFromFile);
    EXPECT_FALSE(result.explicitOverride);
    EXPECT_FALSE(result.gestalt.display.hasPreferredResolution());
    EXPECT_EQ(result.gestalt.shell, ShellKind::Desktop);
    EXPECT_FALSE(result.gestalt.display.scale.has_value());
    EXPECT_TRUE(result.gestalt.display.cutouts.empty());
    EXPECT_FLOAT_EQ(result.gestalt.display.corners.topLeft.radiusX, 0.0f);
}

TEST(GestaltTest, MissingExplicitOverrideIsAnError) {
    ScopedGestaltEnvironment environment;
    setenv("LCL_GESTALT_PATH", "/definitely/not/an/explicit-gestalt.json", 1);
    const auto result = loadGestalt("/unused/platform-default.json");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.explicitOverride);
    EXPECT_NE(result.error.find("explicit Gestalt"), std::string::npos);
}

TEST(GestaltTest, CheckedInGalaxyTabS7ProfileMatchesStrictSchema) {
    const auto profilePath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "devices" / "SM-T870.json";
    std::ifstream profile(profilePath, std::ios::binary);
    ASSERT_TRUE(profile.is_open()) << profilePath;
    std::ostringstream contents;
    contents << profile.rdbuf();

    DeviceGestalt gestalt;
    std::string error;
    ASSERT_TRUE(parseGestaltJson(contents.str(), gestalt, error)) << error;
    EXPECT_EQ(gestalt.name, "Samsung Galaxy Tab S7 (SM-T870)");
    EXPECT_EQ(gestalt.shell, ShellKind::Mobile);
    EXPECT_EQ(gestalt.display.width, 1600u);
    EXPECT_EQ(gestalt.display.height, 2560u);
    EXPECT_EQ(gestalt.display.refreshRateHz, 120u);
    EXPECT_EQ(gestalt.display.scale, 2.0f);
    EXPECT_FLOAT_EQ(gestalt.display.corners.topLeft.radiusX, 28.0f);
}

TEST(GestaltTest, CheckedInMobileDefaultProfileMatchesStrictSchema) {
    const auto profilePath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "config" / "gestalt" / "mobile.json";
    std::ifstream profile(profilePath, std::ios::binary);
    ASSERT_TRUE(profile.is_open()) << profilePath;
    std::ostringstream contents;
    contents << profile.rdbuf();

    DeviceGestalt gestalt;
    std::string error;
    ASSERT_TRUE(parseGestaltJson(contents.str(), gestalt, error)) << error;
    EXPECT_EQ(gestalt.name, "Google Pixel 8 Pro AVD (Pixel_8_Pro)");
    EXPECT_EQ(gestalt.shell, ShellKind::Mobile);
    EXPECT_EQ(gestalt.display.width, 1344u);
    EXPECT_EQ(gestalt.display.height, 2992u);
    EXPECT_EQ(gestalt.display.refreshRateHz, 60u);
    EXPECT_EQ(gestalt.display.scale, 3.0f);
    EXPECT_EQ(gestalt.display.naturalOrientation, NaturalOrientation::Portrait);
    EXPECT_FLOAT_EQ(gestalt.display.safeArea.top, 117.0f);
    EXPECT_FLOAT_EQ(gestalt.display.corners.topLeft.radiusX, 93.0f);
    ASSERT_EQ(gestalt.display.cutouts.size(), 1u);
    EXPECT_FLOAT_EQ(gestalt.display.cutouts.front().x, 631.0f);
    EXPECT_FLOAT_EQ(gestalt.display.cutouts.front().y, 35.0f);
    EXPECT_FLOAT_EQ(gestalt.display.cutouts.front().width, 82.0f);
    EXPECT_FLOAT_EQ(gestalt.display.cutouts.front().height, 82.0f);
}

TEST(GestaltTest, EveryCheckedInDeviceProfileMatchesStrictSchema) {
    const auto deviceDirectory =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "devices";
    size_t profileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(deviceDirectory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;

        std::ifstream profile(entry.path(), std::ios::binary);
        ASSERT_TRUE(profile.is_open()) << entry.path();
        std::ostringstream contents;
        contents << profile.rdbuf();

        DeviceGestalt gestalt;
        std::string error;
        ASSERT_TRUE(parseGestaltJson(contents.str(), gestalt, error))
            << entry.path() << ": " << error;
        EXPECT_FALSE(gestalt.name.empty()) << entry.path();
        EXPECT_EQ(gestalt.shell, ShellKind::Mobile) << entry.path();
        EXPECT_TRUE(gestalt.display.hasPreferredResolution()) << entry.path();
        ++profileCount;
    }
    EXPECT_GE(profileCount, 3u);
}

} // namespace lcl::platform
