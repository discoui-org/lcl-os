#include "core/compositor/surface_transaction_coordinator.hpp"

#include <algorithm>
#include <vector>

namespace lcl::core {

bool SurfaceTransactionCoordinator::begin(
        SurfaceRegistry& surfaces, uint32_t windowId, uint64_t generation,
        const std::vector<SurfaceRegistry::Key>& participants) noexcept {
    if (windowId == 0 || generation == 0 || participants.empty()) return false;
    for (const auto key : participants) {
        const auto found = surfaces.find(key);
        if (found == surfaces.end() || found->second.windowId != windowId) {
            return false;
        }
    }
    // One WindowGroup may raster only one generation at a time. New pointer
    // targets stay in WindowManager until this barrier is promoted and
    // presented; replacing it here would reintroduce raster starvation.
    for (const auto& [_, entry] : surfaces) {
        if (entry.windowId == windowId &&
            entry.atomicConfigureGeneration != 0) {
            return false;
        }
    }
    const auto participantCount = static_cast<uint32_t>(participants.size());
    for (const auto key : participants) {
        auto& entry = surfaces.find(key)->second;
        entry.atomicConfigureGeneration = generation;
        entry.atomicConfigureParticipantCount = participantCount;
        // The barrier is installed only after every configure in the batch has
        // been queued, so participants may raster independently in parallel.
        entry.atomicConfigureIssued = true;
    }
    return true;
}

void SurfaceTransactionCoordinator::cancel(
        SurfaceRegistry& surfaces, uint32_t windowId,
        uint64_t generation) noexcept {
    for (auto& [_, entry] : surfaces) {
        if (entry.windowId != windowId ||
            entry.atomicConfigureGeneration != generation) continue;
        entry.atomicConfigureGeneration = 0;
        entry.atomicConfigureParticipantCount = 0;
        entry.atomicConfigureIssued = false;
    }
}

bool SurfaceTransactionCoordinator::promoteReady(
        SurfaceRegistry& surfaces, const ReadyHandler& readyHandler) {
    struct GroupState {
        uint32_t windowId{0};
        uint64_t generation{0};
        uint32_t expected{0};
        uint32_t seen{0};
        bool ready{true};
    };
    std::vector<GroupState> groups;
    for (const auto& [_, entry] : surfaces) {
        if (entry.atomicConfigureGeneration == 0) continue;
        auto group = std::find_if(
            groups.begin(), groups.end(), [&entry](const GroupState& item) {
                return item.windowId == entry.windowId &&
                    item.generation == entry.atomicConfigureGeneration;
            });
        if (group == groups.end()) {
            groups.push_back({entry.windowId, entry.atomicConfigureGeneration,
                              entry.atomicConfigureParticipantCount});
            group = groups.end() - 1;
        }
        ++group->seen;
        group->expected = std::max(
            group->expected, entry.atomicConfigureParticipantCount);
        group->ready = group->ready && entry.atomicConfigureIssued &&
            entry.configuredGeometryGeneration == group->generation &&
            entry.acceptedConfigureSerial == entry.pendingConfigureSerial &&
            entry.presentationSerial == entry.pendingConfigureSerial;
    }

    bool incomplete = false;
    for (const auto& group : groups) {
        if (!group.ready || group.seen != group.expected) {
            incomplete = true;
            continue;
        }
        // The WindowManager presentation rect is part of the same transaction
        // as the accepted parent/frame layers. Commit it before removing the
        // barrier so the renderer can never observe mixed geometry.
        if (readyHandler &&
            !readyHandler(group.windowId, group.generation)) {
            incomplete = true;
            continue;
        }
        cancel(surfaces, group.windowId, group.generation);
    }
    return incomplete;
}

} // namespace lcl::core
