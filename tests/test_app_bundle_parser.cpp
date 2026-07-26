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
        std::ofstream metaFile(bPath / "metadata.json");
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
        "name": "Test Terminal",
        "version": "1.2.0",
        "icon": "assets/icon.png",
        "executable": "bin/terminal",
        "type": "gui"
    })";

    createMockBundle("Terminal.app", json);
    auto meta = AppBundleParser::parseBundle((tempDir / "Terminal.app").string());

    ASSERT_TRUE(meta.has_value());
    EXPECT_TRUE(meta->valid);
    EXPECT_EQ(meta->name, "Test Terminal");
    EXPECT_EQ(meta->version, "1.2.0");
    EXPECT_EQ(meta->icon, "assets/icon.png");
    EXPECT_EQ(meta->type, "gui");
    EXPECT_EQ(meta->executablePath, (tempDir / "Terminal.app" / "bin/terminal").string());
}

TEST_F(AppBundleParserTest, ScanDirectoryWithMultipleBundles) {
    createMockBundle("AppOne.app", R"({"name": "App One", "executable": "bin/one"})");
    createMockBundle("AppTwo.app", R"({"name": "App Two", "executable": "bin/two"})");
    createMockBundle("NotAnAppDir", R"({"name": "Ignore Me", "executable": "bin/ignore"})");

    auto list = AppBundleParser::scanDirectory(tempDir.string());
    EXPECT_EQ(list.size(), 2u);
}
