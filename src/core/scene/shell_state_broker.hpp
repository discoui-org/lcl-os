#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/scene/focus_controller.hpp"

namespace lcl::core {

/** A monotonic state event that can later be serialized to shell subscribers. */
struct ShellStateDelta {
    enum class Kind : uint8_t {
        SceneAdded,
        SceneUpdated,
        SceneRemoved,
        FocusChanged,
    };

    uint64_t revision{0};
    Kind kind{Kind::SceneUpdated};
    SceneRecord scene{};
    FocusState focus{};
};

struct ShellStateSnapshot {
    uint64_t revision{0};
    std::vector<SceneRecord> scenes;
    FocusState focus{};
};

struct ShellStateDeltaBatch {
    bool requiresSnapshot{false};
    uint64_t revision{0};
    std::vector<ShellStateDelta> deltas;
};

/**
 * Revision source and bounded delta history for shell state.  It deliberately
 * has no socket ownership: Step 6 will add typed shell subscriptions above
 * this broker without giving UI code access to compositor internals.
 */
class ShellStateBroker {
public:
    void publish(const SceneStateChange& change);
    void publish(const FocusState& focus);

    uint64_t revision() const noexcept { return m_revision; }
    ShellStateSnapshot snapshot(const SceneRegistry& scenes,
                                const FocusController& focus) const;
    ShellStateDeltaBatch deltasSince(uint64_t revision) const;

private:
    void append(ShellStateDelta delta);

    static constexpr size_t kMaxRetainedDeltas = 256;
    uint64_t m_revision{0};
    std::vector<ShellStateDelta> m_deltas;
};

} // namespace lcl::core
