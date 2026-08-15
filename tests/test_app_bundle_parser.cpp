#include <gtest/gtest.h>
#include "core/app/app_bundle_parser.hpp"
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
        fs::create_directories(bPath);
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

TEST_F(AppBundleParserTest, ScanDirectoryWithMultipleBundles) {
    createMockBundle("AppOne.app", R"({"id": "org.lcl.one", "name": "App One", "executable": "Executables/one"})");
    createMockBundle("AppTwo.app", R"({"id": "org.lcl.two", "name": "App Two", "executable": "Executables/two"})");
    createMockBundle("NotAnAppDir", R"({"name": "Ignore Me", "executable": "Executables/ignore"})");

    auto list = AppBundleParser::scanDirectory(tempDir.string());
    EXPECT_EQ(list.size(), 2u);
}
