#include "system/security/permission_policy.hpp"

#include <array>

namespace lcl::security {
namespace {

constexpr std::array<PermissionDefinition, 8> kPermissionDefinitions{{
    {"network.client", PermissionEvaluationPhase::LaunchTime, true, false},
    {"files.user-selected", PermissionEvaluationPhase::BrokerTime, false, false},
    {"files.documents.read", PermissionEvaluationPhase::BrokerTime, false, false},
    {"files.documents.write", PermissionEvaluationPhase::BrokerTime, false, false},
    {"clipboard.read", PermissionEvaluationPhase::BrokerTime, false, false},
    {"camera", PermissionEvaluationPhase::BrokerTime, false, false},
    {"microphone", PermissionEvaluationPhase::BrokerTime, false, false},
    {"notifications", PermissionEvaluationPhase::BrokerTime, false, false},
}};

constexpr PermissionDefinition kElevationDefinition{
    "admin.elevation", PermissionEvaluationPhase::BrokerTime, false, true};

} // namespace

const PermissionDefinition* findPermissionDefinition(std::string_view identifier) noexcept {
    for (const PermissionDefinition& definition : kPermissionDefinitions) {
        if (definition.identifier == identifier) {
            return &definition;
        }
    }
    return identifier == kElevationDefinition.identifier ? &kElevationDefinition : nullptr;
}

} // namespace lcl::security
