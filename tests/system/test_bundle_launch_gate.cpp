#include <gtest/gtest.h>

#include "system/security/bundle_launch_gate.hpp"

#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lcl::security {
namespace {

class BundleLaunchGateTest : public ::testing::Test {
protected:
    fs::path temporaryDirectory_;
    BundleRecord record_;

    void SetUp() override {
        temporaryDirectory_ = fs::temp_directory_path() /
                              ("lcl_bundle_launch_gate_" + std::to_string(getpid()));
        fs::remove_all(temporaryDirectory_);
        fs::create_directories(temporaryDirectory_);
        chmod(temporaryDirectory_.c_str(), 0700);

        record_.appId = "org.lcl.example";
        record_.type = "gui";
        record_.runtime = "org.lcl.native";
        record_.files = {BundleFileDigest{"Manifest.json", 0644, sha256("manifest")}};
        record_.digest = digestBundleRecord(record_);
    }

    void TearDown() override {
        fs::remove_all(temporaryDirectory_);
    }

    BundleApprovalStoreConfig approvalStoreConfig() const {
        return {(temporaryDirectory_ / "approvals.db").string(), getuid(), getgid()};
    }
};

TEST_F(BundleLaunchGateTest, FailsClosedUntilAnUnverifiedBundleIsExplicitlyApproved) {
    BundleApprovalStore approvals(approvalStoreConfig());
    BundleLaunchGate gate(approvals);

    const BundleLaunchAssessment pending = gate.assess(
        1000, BundleSourceScope::Direct, record_, BundlePublisherState::Unverified);
    EXPECT_EQ(pending.decision, BundleLaunchDecision::NeedsUserApproval);
    EXPECT_FALSE(pending.allowed());
    EXPECT_NE(pending.reason.find("yayıncısını doğrulayamadı"), std::string::npos);

    std::string error;
    ASSERT_TRUE(approvals.approve(1000, record_, BundlePublisherState::Unverified, error)) << error;
    const BundleLaunchAssessment approved = gate.assess(
        1000, BundleSourceScope::Direct, record_, BundlePublisherState::Unverified);
    EXPECT_EQ(approved.decision, BundleLaunchDecision::AllowUserApproval);
    EXPECT_TRUE(approved.allowed());
}

TEST_F(BundleLaunchGateTest, AllowsVerifiedPublishersWithoutAnApprovalRecord) {
    BundleApprovalStore approvals(approvalStoreConfig());
    BundleLaunchGate gate(approvals);

    const BundleLaunchAssessment assessment = gate.assess(
        1000, BundleSourceScope::Machine, record_, BundlePublisherState::SignatureVerified);

    EXPECT_EQ(assessment.decision, BundleLaunchDecision::AllowSignature);
    EXPECT_TRUE(assessment.allowed());
}

TEST_F(BundleLaunchGateTest, OnlySystemScopeMayUseSystemImageTrust) {
    BundleApprovalStore approvals(approvalStoreConfig());
    BundleLaunchGate gate(approvals);

    const BundleLaunchAssessment system = gate.assess(
        1000, BundleSourceScope::System, record_, BundlePublisherState::SystemImageTrusted);
    EXPECT_EQ(system.decision, BundleLaunchDecision::AllowSystemImage);

    const BundleLaunchAssessment machine = gate.assess(
        1000, BundleSourceScope::Machine, record_, BundlePublisherState::SystemImageTrusted);
    EXPECT_EQ(machine.decision, BundleLaunchDecision::Reject);
}

TEST_F(BundleLaunchGateTest, RejectsRootDesktopUserAndInvalidRecords) {
    BundleApprovalStore approvals(approvalStoreConfig());
    BundleLaunchGate gate(approvals);

    EXPECT_EQ(gate.assess(0, BundleSourceScope::Direct, record_, BundlePublisherState::Unverified).decision,
              BundleLaunchDecision::Reject);
    record_.digest = {};
    EXPECT_EQ(gate.assess(1000, BundleSourceScope::Direct, record_, BundlePublisherState::Unverified).decision,
              BundleLaunchDecision::Reject);
}

} // namespace
} // namespace lcl::security
