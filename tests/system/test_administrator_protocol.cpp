#include <gtest/gtest.h>

#include "system/security/administrator_protocol.hpp"

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

} // namespace
} // namespace lcl::security
