#include <gtest/gtest.h>

#include "core/shell/shell_state_model.hpp"

namespace lcl::shell {
namespace {

ShellScene scene(uint64_t id, const char* appId, protocol::LCLSceneVisibility visibility) {
    ShellScene value{};
    value.sceneId = id;
    value.appId = appId;
    value.title = appId;
    value.visibility = visibility;
    return value;
}

} // namespace

TEST(ShellStateModelTest, MobileRecentsUsesOneNonClosingItemPerAuthoritativeScene) {
    ShellStateModel model;
    ShellStateSnapshot snapshot{};
    snapshot.revision = 10;
    snapshot.activeSceneId = 2;
    snapshot.scenes = {
        scene(1, "org.lcl.terminal", protocol::LCLSceneVisibility::Visible),
        scene(2, "org.lcl.terminal", protocol::LCLSceneVisibility::Minimized),
        scene(3, "org.lcl.demo", protocol::LCLSceneVisibility::Closing),
    };
    ASSERT_TRUE(model.applySnapshot(snapshot));

    const auto recents = model.recents();
    ASSERT_EQ(recents.size(), 2u);
    EXPECT_EQ(recents[0].sceneId, 1u);
    EXPECT_EQ(recents[1].sceneId, 2u);
    EXPECT_EQ(recents[0].appId, recents[1].appId);

    ShellStateDelta remove{};
    remove.revision = 11;
    remove.kind = protocol::LCLShellStateDeltaKind::SceneRemoved;
    remove.activeSceneId = 1;
    remove.scene = scene(2, "org.lcl.terminal", protocol::LCLSceneVisibility::Minimized);
    ASSERT_TRUE(model.applyDelta(remove));
    ASSERT_EQ(model.recents().size(), 1u);
    EXPECT_EQ(model.recents().front().sceneId, 1u);
}

TEST(ShellStateModelTest, RejectsDeltaWithoutItsPrecedingRevision) {
    ShellStateModel model;
    ShellStateSnapshot snapshot{};
    snapshot.revision = 4;
    ASSERT_TRUE(model.applySnapshot(snapshot));

    ShellStateDelta skipped{};
    skipped.revision = 6;
    skipped.kind = protocol::LCLShellStateDeltaKind::FocusChanged;
    EXPECT_FALSE(model.applyDelta(skipped));
    EXPECT_EQ(model.snapshot().revision, 4u);
}

} // namespace lcl::shell
