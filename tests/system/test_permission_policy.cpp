#include <gtest/gtest.h>

#include "system/security/permission_policy.hpp"

namespace lcl::security {
namespace {

TEST(PermissionPolicyTest, HasAClosedVocabularyWithExplicitEnforcementPhase) {
    const PermissionDefinition* network = findPermissionDefinition("network.client");
    ASSERT_NE(network, nullptr);
    EXPECT_EQ(network->evaluationPhase, PermissionEvaluationPhase::LaunchTime);
    EXPECT_TRUE(network->requiresProcessRestart);
    EXPECT_FALSE(network->requiresVisibleConsent);

    const PermissionDefinition* gpu = findPermissionDefinition("graphics.gpu");
    ASSERT_NE(gpu, nullptr);
    EXPECT_EQ(gpu->evaluationPhase, PermissionEvaluationPhase::LaunchTime);
    EXPECT_TRUE(gpu->requiresProcessRestart);
    EXPECT_FALSE(gpu->requiresVisibleConsent);

    const PermissionDefinition* documents = findPermissionDefinition("files.documents.write");
    ASSERT_NE(documents, nullptr);
    EXPECT_EQ(documents->evaluationPhase, PermissionEvaluationPhase::BrokerTime);
    EXPECT_FALSE(documents->requiresProcessRestart);
    EXPECT_FALSE(documents->requiresVisibleConsent);

    const PermissionDefinition* elevation = findPermissionDefinition("admin.elevation");
    ASSERT_NE(elevation, nullptr);
    EXPECT_EQ(elevation->evaluationPhase, PermissionEvaluationPhase::BrokerTime);
    EXPECT_FALSE(elevation->requiresProcessRestart);
    EXPECT_TRUE(elevation->requiresVisibleConsent);

    EXPECT_EQ(findPermissionDefinition("network.server"), nullptr);
    EXPECT_EQ(findPermissionDefinition("org.example.hidden-power"), nullptr);
}

} // namespace
} // namespace lcl::security
