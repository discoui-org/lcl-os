#include <gtest/gtest.h>
#include "system/session/app_bundle_parser.hpp"
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

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

    fs::path createMockBundle(const std::string& bundleName, const std::string& jsonContent) {
        const fs::path bPath = tempDir / bundleName;
        fs::create_directories(bPath / "Resources");
        std::ofstream iconFile(bPath / "Resources" / "Icon.png");
        iconFile << "test";
        iconFile.close();
        std::ofstream metaFile(bPath / "Manifest.json");
        metaFile << jsonContent;
        metaFile.close();
        return bPath;
    }

    void createExecutable(const fs::path& bundle, const std::string& relativePath,
                          bool executable = true) {
        const fs::path executablePath = bundle / relativePath;
        fs::create_directories(executablePath.parent_path());
        std::ofstream executableFile(executablePath);
        executableFile << "#!/bin/sh\nexit 0\n";
        executableFile.close();
        if (executable) {
            chmod(executablePath.c_str(), 0755);
        }
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

    const fs::path bundle = createMockBundle("Terminal.app", json);
    createExecutable(bundle, "Executables/Terminal");
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

TEST_F(AppBundleParserTest, ParsesRequestedPermissionsAsDeclarations) {
    const fs::path bundle = createMockBundle(
        "Permissioned.app",
        R"({"id":"org.lcl.permissioned","name":"Permissioned","icon":"Resources/Icon.png","executable":"Executables/app","requestedPermissions":["network.client","files.user-selected"]})");
    createExecutable(bundle, "Executables/app");

    const auto metadata = AppBundleParser::parseBundle(bundle.string());

    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->requestedPermissions,
              std::vector<std::string>({"network.client", "files.user-selected"}));
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

TEST_F(AppBundleParserTest, RejectsMalformedOrDuplicateManifestFields) {
    createMockBundle(
        "Malformed.app",
        R"({"id":"org.lcl.malformed","name":"Broken","icon":"Resources/Icon.png",)" );
    createMockBundle(
        "Duplicate.app",
        R"({"id":"org.lcl.duplicate","id":"org.lcl.other","name":"Duplicate","icon":"Resources/Icon.png","executable":"Executables/app"})" );

    EXPECT_FALSE(AppBundleParser::parseBundle((tempDir / "Malformed.app").string()).has_value());
    EXPECT_FALSE(AppBundleParser::parseBundle((tempDir / "Duplicate.app").string()).has_value());
}

TEST_F(AppBundleParserTest, RejectsExecutablePathsOutsideTheBundle) {
    createMockBundle(
        "Absolute.app",
        R"({"id":"org.lcl.absolute","name":"Absolute","icon":"Resources/Icon.png","executable":"/System/Core/lcl-core"})" );
    createMockBundle(
        "Traversal.app",
        R"({"id":"org.lcl.traversal","name":"Traversal","icon":"Resources/Icon.png","executable":"Executables/../../outside"})" );

    EXPECT_FALSE(AppBundleParser::parseBundle((tempDir / "Absolute.app").string()).has_value());
    EXPECT_FALSE(AppBundleParser::parseBundle((tempDir / "Traversal.app").string()).has_value());
}

TEST_F(AppBundleParserTest, RejectsExecutableSymlinkEscapingBundle) {
    const fs::path externalExecutable = tempDir / "outside-program";
    std::ofstream externalFile(externalExecutable);
    externalFile << "#!/bin/sh\nexit 0\n";
    externalFile.close();
    chmod(externalExecutable.c_str(), 0755);

    const fs::path bundle = createMockBundle(
        "EscapingSymlink.app",
        R"({"id":"org.lcl.escaping-symlink","name":"Escaping","icon":"Resources/Icon.png","executable":"Executables/app"})" );
    fs::create_directories(bundle / "Executables");
    fs::create_symlink(externalExecutable, bundle / "Executables" / "app");

    EXPECT_FALSE(AppBundleParser::parseBundle(bundle.string()).has_value());
}

TEST_F(AppBundleParserTest, RejectsInvalidOrDuplicateRequestedPermissions) {
    const fs::path invalidBundle = createMockBundle(
        "InvalidPermission.app",
        R"({"id":"org.lcl.invalid-permission","name":"Invalid Permission","icon":"Resources/Icon.png","executable":"Executables/app","requestedPermissions":["Network.Client"]})" );
    createExecutable(invalidBundle, "Executables/app");
    const fs::path duplicateBundle = createMockBundle(
        "DuplicatePermission.app",
        R"({"id":"org.lcl.duplicate-permission","name":"Duplicate Permission","icon":"Resources/Icon.png","executable":"Executables/app","requestedPermissions":["network.client","network.client"]})" );
    createExecutable(duplicateBundle, "Executables/app");

    EXPECT_FALSE(AppBundleParser::parseBundle(invalidBundle.string()).has_value());
    EXPECT_FALSE(AppBundleParser::parseBundle(duplicateBundle.string()).has_value());
}

TEST_F(AppBundleParserTest, ScanDirectoryWithMultipleBundles) {
    const fs::path appOne = createMockBundle("AppOne.app", R"({"id": "org.lcl.one", "name": "App One", "icon": "Resources/Icon.png", "executable": "Executables/one"})");
    const fs::path appTwo = createMockBundle("AppTwo.app", R"({"id": "org.lcl.two", "name": "App Two", "icon": "Resources/Icon.png", "executable": "Executables/two"})");
    createMockBundle("NotAnAppDir", R"({"name": "Ignore Me", "executable": "Executables/ignore"})");
    createExecutable(appOne, "Executables/one");
    createExecutable(appTwo, "Executables/two");

    auto list = AppBundleParser::scanDirectory(tempDir.string());
    EXPECT_EQ(list.size(), 2u);
}
