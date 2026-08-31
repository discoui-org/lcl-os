#include <gtest/gtest.h>

#include "system/security/security_admin_protocol.hpp"

namespace lcl::security {
namespace {

PendingBundleApproval validPendingApproval() {
    PendingBundleApproval request{};
    request.userUid = 1000;
    request.appId = "org.lcl.example";
    request.bundleRecordDigest = sha256("example-record");
    request.publisherState = BundlePublisherState::Unverified;
    request.sourceScope = BundleSourceScope::Direct;
    request.bundlePath = "/Users/Rei/Downloads/Example.app";
    request.displayName = "Example";
    request.appVersion = "1.0";
    return request;
}

TEST(SecurityAdminProtocolTest, RoundTripsAUserScopedPendingBundleList) {
    const PendingBundleApproval expected = validPendingApproval();
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodePendingBundleApprovals({expected}, payload));

    SecurityAdminHeader header{};
    header.opcode = SecurityAdminOpcode::PendingBundleApprovalsResponse;
    header.requestId = 7;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(encodeSecurityAdminPacket(header, payload, packet));

    DecodedSecurityAdminPacket decoded{};
    ASSERT_TRUE(decodeSecurityAdminPacket(packet.data(), packet.size(), decoded));
    ASSERT_EQ(decoded.header.opcode, SecurityAdminOpcode::PendingBundleApprovalsResponse);
    ASSERT_EQ(decoded.header.requestId, 7u);
    std::vector<PendingBundleApproval> pending;
    ASSERT_TRUE(decodePendingBundleApprovals(decoded.payload, pending));
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front().userUid, expected.userUid);
    EXPECT_EQ(pending.front().appId, expected.appId);
    EXPECT_EQ(pending.front().bundleRecordDigest, expected.bundleRecordDigest);
    EXPECT_EQ(pending.front().bundlePath, expected.bundlePath);
    EXPECT_EQ(pending.front().displayName, expected.displayName);
}

TEST(SecurityAdminProtocolTest, RoundTripsExactApprovalIdentityAndResult) {
    const SecurityAdminBundleIdentity identity{
        .appId = "org.lcl.example",
        .bundleRecordDigest = sha256("example-record"),
    };
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeSecurityAdminBundleIdentity(identity, payload));
    SecurityAdminBundleIdentity decodedIdentity{};
    ASSERT_TRUE(decodeSecurityAdminBundleIdentity(payload, decodedIdentity));
    EXPECT_EQ(decodedIdentity.appId, identity.appId);
    EXPECT_EQ(decodedIdentity.bundleRecordDigest, identity.bundleRecordDigest);

    const SecurityAdminApprovalResult expected{.accepted = true, .message = "approved"};
    ASSERT_TRUE(encodeSecurityAdminApprovalResult(expected, payload));
    SecurityAdminApprovalResult decodedResult{};
    ASSERT_TRUE(decodeSecurityAdminApprovalResult(payload, decodedResult));
    EXPECT_TRUE(decodedResult.accepted);
    EXPECT_EQ(decodedResult.message, "approved");
}

TEST(SecurityAdminProtocolTest, RejectsMalformedOrUnscopedPackets) {
    SecurityAdminBundleIdentity invalid{};
    invalid.appId = "org.lcl.example";
    std::vector<std::uint8_t> payload;
    EXPECT_FALSE(encodeSecurityAdminBundleIdentity(invalid, payload));

    SecurityAdminHeader header{};
    header.opcode = SecurityAdminOpcode::ApproveBundleRequest;
    header.requestId = 0;
    EXPECT_FALSE(encodeSecurityAdminPacket(header, {}, payload));

    const PendingBundleApproval invalidPending{
        .userUid = 0,
        .appId = "org.lcl.example",
        .bundleRecordDigest = sha256("record"),
        .publisherState = BundlePublisherState::Unverified,
        .sourceScope = BundleSourceScope::Direct,
        .bundlePath = "/Users/Rei/Example.app",
        .displayName = "Example",
        .appVersion = "1.0",
    };
    EXPECT_FALSE(encodePendingBundleApprovals({invalidPending}, payload));
}

} // namespace
} // namespace lcl::security
