#pragma once

#include <vector>

#include "core/shell/shell_state_client.hpp"

namespace lcl::shell {

/**
 * Widget-independent local materialization of the revisioned shell stream.
 * Desktop surfaces may project it by application; mobile Recents may project
 * one non-closing scene per card without creating a second state authority.
 */
class ShellStateModel {
public:
    bool applySnapshot(const ShellStateSnapshot& snapshot);
    bool applyDelta(const ShellStateDelta& delta);

    bool hasSnapshot() const noexcept { return m_hasSnapshot; }
    const ShellStateSnapshot& snapshot() const noexcept { return m_snapshot; }
    std::vector<ShellScene> recents() const;

private:
    bool m_hasSnapshot{false};
    ShellStateSnapshot m_snapshot;
};

} // namespace lcl::shell
