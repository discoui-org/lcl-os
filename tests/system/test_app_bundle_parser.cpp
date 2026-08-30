#include <gtest/gtest.h>
#include "system/session/app_bundle_parser.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
using namespace lcl::core;

class AppBundleParserTest : public ::testing::Test {
protected:
    fs::path tempDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() / "lcl_test_app_bundles";
        fs::create_directories(tempDir);
    }

    void TearDown() override {
        fs::remove_all(tempDir);
    }

    void createMockBundle(const std::string& bundleName, const std::string& jsonContent) {
        fs::path bPath = tempDir / bundleName;
        fs::create_directories(bPath / "Resources");
        std::ofstream iconFile(bPath / "Resources" / "Icon.png");
        iconFile << "test";
        iconFile.close();
        std::ofstream metaFile(bPath / "Manifest.json");
        metaFile << jsonContent;
        metaFile.close();
    }
};

TEST_F(AppBundleParserTest, ParseInvalidNonExistentPath) {
    auto meta = AppBundleParser::parseBundle((tempDir / "NonExistent.app").string());
    EXPECT_FALSE(meta.has_value());
}

TEST_F(AppBundleParserTest, ParseValidAppBundle) {
    std::string json = R"({
        "id": "org.lcl.terminal",
        "name": "Test Terminal",
        "version": "1.2.0",
        "icon": "Resources/Icon.png",
        "executable": "Executables/Terminal",
        "runtime": "org.lcl.native",
        "type": "gui"
    })";

    createMockBundle("Terminal.app", json);
    auto meta = AppBundleParser::parseBundle((tempDir / "Terminal.app").string());

    ASSERT_TRUE(meta.has_value());
    EXPECT_TRUE(meta->valid);
    EXPECT_EQ(meta->appId, "org.lcl.terminal");
    EXPECT_EQ(meta->name, "Test Terminal");
    EXPECT_EQ(meta->version, "1.2.0");
    EXPECT_EQ(meta->icon, "Resources/Icon.png");
    EXPECT_EQ(meta->type, "gui");
    EXPECT_EQ(meta->runtime, "org.lcl.native");
    EXPECT_EQ(meta->executablePath, (tempDir / "Terminal.app" / "Executables/Terminal").string());
}

TEST_F(AppBundleParserTest, RejectsLegacyMetadataOnlyBundle) {
    const fs::path bundle = tempDir / "Legacy.app";
    fs::create_directories(bundle / "assets");
    std::ofstream legacy(bundle / "metadata.json");
    legacy << R"({"id":"org.lcl.legacy","name":"Legacy","executable":"bin/legacy"})";
    legacy.close();

    EXPECT_FALSE(AppBundleParser::parseBundle(bundle.string()).has_value());
}

TEST_F(AppBundleParserTest, RejectsManifestWithoutCanonicalId) {
    createMockBundle(
        "MissingId.app",
        R"({"name":"Missing ID","executable":"Executables/app"})");

    EXPECT_FALSE(
        AppBundleParser::parseBundle((tempDir / "MissingId.app").string()).has_value());
}

TEST_F(AppBundleParserTest, RejectsManifestWithoutResourcesDirectory) {
    const fs::path bundle = tempDir / "NoResources.app";
    fs::create_directories(bundle);
    std::ofstream manifest(bundle / "Manifest.json");
    manifest << R"({"id":"org.lcl.no-resources","name":"No Resources","executable":"Executables/app"})";
    manifest.close();

    EXPECT_FALSE(AppBundleParser::parseBundle(bundle.string()).has_value());
}

TEST_F(AppBundleParserTest, ScanDirectoryWithMultipleBundles) {
    createMockBundle("AppOne.app", R"({"id": "org.lcl.one", "name": "App One", "icon": "Resources/Icon.png", "executable": "Executables/one"})");
    createMockBundle("AppTwo.app", R"({"id": "org.lcl.two", "name": "App Two", "icon": "Resources/Icon.png", "executable": "Executables/two"})");
    createMockBundle("NotAnAppDir", R"({"name": "Ignore Me", "executable": "Executables/ignore"})");

    auto list = AppBundleParser::scanDirectory(tempDir.string());
    EXPECT_EQ(list.size(), 2u);
}
