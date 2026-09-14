#include <gtest/gtest.h>

#include "system/security/administrator_daemon.hpp"
#include "system/security/administrator_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace lcl::security {
namespace {

TEST(AdministratorProtocolTest, RoundTripsArgumentVectorWithoutAShellCommand) {
    const AdministratorExecuteRequest expected{
        .workingDirectory = "/Users/Rei",
        .arguments = {"id", "-u"},
    };
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeAdministratorExecuteRequest(expected, payload));
    AdministratorExecuteRequest decoded{};
    ASSERT_TRUE(decodeAdministratorExecuteRequest(payload, decoded));
    EXPECT_EQ(decoded.workingDirectory, expected.workingDirectory);
    EXPECT_EQ(decoded.arguments, expected.arguments);

    AdministratorHeader header{};
    header.opcode = AdministratorOpcode::ExecuteRequest;
    header.requestId = 41;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(encodeAdministratorPacket(header, payload, packet));
    DecodedAdministratorPacket decodedPacket{};
    ASSERT_TRUE(decodeAdministratorPacket(packet.data(), packet.size(), decodedPacket));
    EXPECT_EQ(decodedPacket.header.opcode, AdministratorOpcode::ExecuteRequest);
    EXPECT_EQ(decodedPacket.header.requestId, 41u);
}

TEST(AdministratorProtocolTest, RoundTripsSharedPermissionPromptAndDecision) {
    const AdministratorPrompt expected{
        .appName = "Terminal",
        .title = "Wants administrator privileges",
        .description = "Allow Terminal to perform this protected command?",
    };
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeAdministratorPrompt(expected, payload));
    AdministratorPrompt decoded{};
    ASSERT_TRUE(decodeAdministratorPrompt(payload, decoded));
    EXPECT_EQ(decoded.appName, expected.appName);
    EXPECT_EQ(decoded.title, expected.title);
    EXPECT_EQ(decoded.description, expected.description);

    ASSERT_TRUE(encodeAdministratorDecision(true, payload));
    bool allowed = false;
    ASSERT_TRUE(decodeAdministratorDecision(payload, allowed));
    EXPECT_TRUE(allowed);
}

TEST(AdministratorProtocolTest, RoundTripsBinaryOutputResultAndError) {
    const AdministratorCommandOutput expected{
        .stream = AdministratorOutputStream::StandardError,
        .bytes = {'a', 0, 'b', '\n'},
    };
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeAdministratorCommandOutput(expected, payload));
    AdministratorCommandOutput decoded{};
    ASSERT_TRUE(decodeAdministratorCommandOutput(payload, decoded));
    EXPECT_EQ(decoded.stream, expected.stream);
    EXPECT_EQ(decoded.bytes, expected.bytes);

    ASSERT_TRUE(encodeAdministratorCommandResult(127, payload));
    int exitCode = 0;
    ASSERT_TRUE(decodeAdministratorCommandResult(payload, exitCode));
    EXPECT_EQ(exitCode, 127);

    ASSERT_TRUE(encodeAdministratorError("denied", payload));
    std::string error;
    ASSERT_TRUE(decodeAdministratorError(payload, error));
    EXPECT_EQ(error, "denied");
}

TEST(AdministratorProtocolTest, RejectsInteractiveAndMalformedRequests) {
    std::vector<std::uint8_t> payload;
    EXPECT_FALSE(encodeAdministratorExecuteRequest(
        {.workingDirectory = "/Users/Rei", .arguments = {}}, payload));
    EXPECT_FALSE(encodeAdministratorExecuteRequest(
        {.workingDirectory = "relative", .arguments = {"id"}}, payload));
    EXPECT_FALSE(encodeAdministratorExecuteRequest(
        {.workingDirectory = "/Users/Rei", .arguments = {"sh", "line\n"}}, payload));
    EXPECT_FALSE(encodeAdministratorCommandResult(256, payload));
    bool allowed = false;
    EXPECT_FALSE(decodeAdministratorDecision({2}, allowed));

    AdministratorHeader header{};
    header.opcode = static_cast<AdministratorOpcode>(999);
    header.requestId = 1;
    EXPECT_FALSE(encodeAdministratorPacket(header, {}, payload));
}

TEST(AdministratorProtocolTest, EncodesTrustedShellSessionRevoke) {
    AdministratorHeader header{};
    header.opcode = AdministratorOpcode::RevokeSession;
    header.requestId = 1;
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(encodeAdministratorPacket(header, {}, packet));
    DecodedAdministratorPacket decoded{};
    ASSERT_TRUE(decodeAdministratorPacket(packet.data(), packet.size(), decoded));
    EXPECT_EQ(decoded.header.opcode, AdministratorOpcode::RevokeSession);
    EXPECT_TRUE(decoded.payload.empty());
}

TEST(AdministratorPermissionLeaseTest, IsBoundToExactTerminalLifetimeAndExpires) {
    AdministratorPermissionLease lease;
    const auto now = std::chrono::steady_clock::now();
    const AdministratorTerminalIdentity terminal{42, 9001};
    EXPECT_FALSE(lease.permits(terminal, now));
    lease.grant(terminal, now, std::chrono::minutes(5));
    EXPECT_TRUE(lease.permits(terminal, now + std::chrono::minutes(4)));
    EXPECT_FALSE(lease.permits({43, 9001}, now + std::chrono::minutes(1)));
    EXPECT_FALSE(lease.permits({42, 9002}, now + std::chrono::minutes(1)));
    EXPECT_FALSE(lease.permits(terminal, now + std::chrono::minutes(5)));
    lease.revoke();
    EXPECT_FALSE(lease.active());
}

TEST(AdministratorAuditStoreTest, WritesOwnerOnlyInjectionSafeCommandRecord) {
    char directoryTemplate[] = "/tmp/lcl-administrator-audit-XXXXXX";
    const char* directory = mkdtemp(directoryTemplate);
    ASSERT_NE(directory, nullptr);
    const std::filesystem::path root(directory);
    const auto auditPath = root / "admin-audit.v1";
    AdministratorAuditStore store({auditPath.string(), geteuid(), getegid()});
    std::string error;
    ASSERT_TRUE(store.append("decision", 1000, {42, 9001}, 7,
                             {"tool", "field\tbreak", "line\nbreak"},
                             true, "user-approved", error)) << error;
    struct stat status {};
    ASSERT_EQ(stat(auditPath.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    std::ifstream input(auditPath);
    const std::string record((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    EXPECT_NE(record.find("\torg.lcl.terminal\t42\t9001\t7\tallow\t"),
              std::string::npos);
    EXPECT_NE(record.find("field%09break"), std::string::npos);
    EXPECT_NE(record.find("line%0abreak"), std::string::npos);
    EXPECT_EQ(std::count(record.begin(), record.end(), '\n'), 1);
    std::filesystem::remove_all(root);
}

TEST(AdministratorAuditStoreTest, RejectsSymlinkAuditTarget) {
    char directoryTemplate[] = "/tmp/lcl-administrator-symlink-XXXXXX";
    const char* directory = mkdtemp(directoryTemplate);
    ASSERT_NE(directory, nullptr);
    const std::filesystem::path root(directory);
    const auto target = root / "target";
    const auto auditPath = root / "admin-audit.v1";
    std::ofstream(target) << "existing\n";
    ASSERT_EQ(symlink(target.c_str(), auditPath.c_str()), 0);
    AdministratorAuditStore store({auditPath.string(), geteuid(), getegid()});
    std::string error;
    EXPECT_FALSE(store.append("decision", 1000, {42, 9001}, 7, {"id"},
                              true, "user-approved", error));
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace lcl::security
