#include <gtest/gtest.h>

#include "core/retained_render_tree.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/filter_group.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

using lcl::ui::BackdropSurface;
using lcl::ui::Container;
using lcl::ui::FilterGroup;
using lcl::ui::ScrollView;
using lcl::ui::detail::RenderBoundaryReason;
using lcl::ui::detail::RenderNodeCompiler;
using lcl::ui::detail::RenderNodeDisposition;
using lcl::ui::detail::RenderTreeDiffer;
using lcl::ui::detail::RetainedRenderNodeUpdate;
using lcl::ui::detail::hasBoundaryReason;

const lcl::ui::detail::RetainedRenderNode* findRetainedNode(
        const lcl::ui::detail::RenderTree& tree, uint64_t id) {
    for (const auto& node : tree.retainedNodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const RetainedRenderNodeUpdate* findUpdate(
        const lcl::ui::detail::RenderTreeTransaction& transaction,
        uint64_t id) {
    for (const auto& update : transaction.updates) {
        if (update.node.id == id) return &update;
    }
    return nullptr;
}

TEST(RetainedRenderTreeTest, PreservesStablePreorderAndFlattensOrdinaryWidgets) {
    Container root;
    auto child = std::make_unique<Container>();
    auto grandchild = std::make_unique<Container>();
    const uint64_t childId = child->getObjectId();
    const uint64_t grandchildId = grandchild->getObjectId();
    child->addChild(std::move(grandchild));
    root.addChild(std::move(child));

    const RenderNodeCompiler compiler;
    const auto first = compiler.compile(root);
    const auto second = compiler.compile(root, first.nodes.size());

    ASSERT_EQ(first.nodes.size(), 3u);
    ASSERT_EQ(second.nodes.size(), first.nodes.size());
    EXPECT_EQ(first.rootId, root.getObjectId());
    EXPECT_EQ(first.nodes[0].id, root.getObjectId());
    EXPECT_EQ(first.nodes[1].id, childId);
    EXPECT_EQ(first.nodes[2].id, grandchildId);

    for (std::size_t index = 0; index < first.nodes.size(); ++index) {
        EXPECT_EQ(second.nodes[index].id, first.nodes[index].id);
        EXPECT_EQ(second.nodes[index].parentId, first.nodes[index].parentId);
    }

    EXPECT_EQ(first.nodes[0].disposition, RenderNodeDisposition::Retained);
    EXPECT_TRUE(hasBoundaryReason(first.nodes[0].boundaryReasons,
                                  RenderBoundaryReason::Root));
    EXPECT_EQ(first.nodes[1].disposition, RenderNodeDisposition::Flattened);
    EXPECT_EQ(first.nodes[2].disposition, RenderNodeDisposition::Flattened);
    EXPECT_EQ(first.retainedNodeCount(), 1u);
    ASSERT_EQ(first.nodes[0].children.size(), 1u);
    EXPECT_EQ(first.nodes[0].children[0], childId);
    ASSERT_EQ(first.nodes[1].children.size(), 1u);
    EXPECT_EQ(first.nodes[1].children[0], grandchildId);
    EXPECT_TRUE(first.nodes[2].children.empty());
}

TEST(RetainedRenderTreeTest, MarksScrollViewportAndDirectContentBoundaries) {
    Container root;
    auto scroll = std::make_unique<ScrollView>();
    auto content = std::make_unique<Container>();
    auto row = std::make_unique<Container>();
    const uint64_t scrollId = scroll->getObjectId();
    const uint64_t contentId = content->getObjectId();
    const uint64_t rowId = row->getObjectId();
    content->addChild(std::move(row));
    scroll->setContent(std::move(content));
    root.addChild(std::move(scroll));

    const auto tree = RenderNodeCompiler{}.compile(root);
    const auto* viewportNode = tree.find(scrollId);
    const auto* contentNode = tree.find(contentId);
    const auto* rowNode = tree.find(rowId);

    ASSERT_NE(viewportNode, nullptr);
    ASSERT_NE(contentNode, nullptr);
    ASSERT_NE(rowNode, nullptr);
    EXPECT_EQ(viewportNode->disposition, RenderNodeDisposition::Retained);
    EXPECT_TRUE(hasBoundaryReason(viewportNode->boundaryReasons,
                                  RenderBoundaryReason::ScrollViewport));
    EXPECT_TRUE(hasBoundaryReason(viewportNode->boundaryReasons,
                                  RenderBoundaryReason::Clip));
    EXPECT_TRUE(viewportNode->clipBounds.has_value());

    EXPECT_EQ(contentNode->parentId, scrollId);
    EXPECT_EQ(contentNode->disposition, RenderNodeDisposition::Retained);
    EXPECT_TRUE(hasBoundaryReason(contentNode->boundaryReasons,
                                  RenderBoundaryReason::ScrollContent));
    EXPECT_EQ(rowNode->parentId, contentId);
    EXPECT_EQ(rowNode->disposition, RenderNodeDisposition::Flattened);
    EXPECT_EQ(tree.retainedNodeCount(), 3u);
}

TEST(RetainedRenderTreeTest, SeparatesPresentationAndEffectBoundaries) {
    Container root;
    auto presentation = std::make_unique<Container>();
    presentation->setTranslation(8.0f, -3.0f);
    presentation->setOpacity(0.6f);
    presentation->setClipsToBounds(true);
    const uint64_t presentationId = presentation->getObjectId();

    auto backdrop = std::make_unique<BackdropSurface>();
    const uint64_t backdropId = backdrop->getObjectId();
    auto filterGroup = std::make_unique<FilterGroup>();
    const uint64_t filterGroupId = filterGroup->getObjectId();

    root.addChild(std::move(presentation));
    root.addChild(std::move(backdrop));
    root.addChild(std::move(filterGroup));

    const auto tree = RenderNodeCompiler{}.compile(root);
    const auto* presentationNode = tree.find(presentationId);
    const auto* backdropNode = tree.find(backdropId);
    const auto* filterGroupNode = tree.find(filterGroupId);

    ASSERT_NE(presentationNode, nullptr);
    EXPECT_EQ(presentationNode->disposition, RenderNodeDisposition::Retained);
    EXPECT_TRUE(hasBoundaryReason(presentationNode->boundaryReasons,
                                  RenderBoundaryReason::Transform));
    EXPECT_TRUE(hasBoundaryReason(presentationNode->boundaryReasons,
                                  RenderBoundaryReason::Opacity));
    EXPECT_TRUE(hasBoundaryReason(presentationNode->boundaryReasons,
                                  RenderBoundaryReason::Clip));
    EXPECT_FLOAT_EQ(presentationNode->presentation.translationX, 8.0f);
    EXPECT_FLOAT_EQ(presentationNode->presentation.translationY, -3.0f);
    EXPECT_FLOAT_EQ(presentationNode->presentation.opacity, 0.6f);

    ASSERT_NE(backdropNode, nullptr);
    EXPECT_TRUE(hasBoundaryReason(backdropNode->boundaryReasons,
                                  RenderBoundaryReason::Effect));
    ASSERT_NE(filterGroupNode, nullptr);
    EXPECT_TRUE(hasBoundaryReason(filterGroupNode->boundaryReasons,
                                  RenderBoundaryReason::Effect));

    const auto* retainedPresentation = findRetainedNode(tree, presentationId);
    const auto* retainedBackdrop = findRetainedNode(tree, backdropId);
    const auto* retainedFilterGroup = findRetainedNode(tree, filterGroupId);
    ASSERT_NE(retainedPresentation, nullptr);
    ASSERT_NE(retainedBackdrop, nullptr);
    ASSERT_NE(retainedFilterGroup, nullptr);
    EXPECT_EQ(retainedPresentation->siblingIndex, 0u);
    EXPECT_EQ(retainedBackdrop->siblingIndex, 1u);
    EXPECT_EQ(retainedFilterGroup->siblingIndex, 2u);
}

TEST(RetainedRenderTreeTest, KeepsIdentityStableAcrossContentAndPropertyChanges) {
    Container root;
    auto child = std::make_unique<Container>();
    Container* childPtr = child.get();
    const uint64_t childId = child->getObjectId();
    root.addChild(std::move(child));

    const RenderNodeCompiler compiler;
    const auto initial = compiler.compile(root);
    const auto* initialChild = initial.find(childId);
    ASSERT_NE(initialChild, nullptr);
    const uint64_t initialContentRevision = initialChild->contentRevision;
    const uint64_t initialPropertyRevision = initialChild->propertyRevision;

    childPtr->markDirty();
    const auto contentChanged = compiler.compile(root);
    const auto* contentChangedChild = contentChanged.find(childId);
    ASSERT_NE(contentChangedChild, nullptr);
    EXPECT_GT(contentChangedChild->contentRevision, initialContentRevision);
    EXPECT_EQ(contentChangedChild->propertyRevision, initialPropertyRevision);
    EXPECT_EQ(contentChangedChild->disposition, RenderNodeDisposition::Flattened);

    childPtr->setTranslationX(5.0f);
    const auto propertyChanged = compiler.compile(root);
    const auto* propertyChangedChild = propertyChanged.find(childId);
    ASSERT_NE(propertyChangedChild, nullptr);
    EXPECT_EQ(propertyChangedChild->id, childId);
    EXPECT_GT(propertyChangedChild->propertyRevision,
              contentChangedChild->propertyRevision);
    EXPECT_EQ(propertyChangedChild->disposition, RenderNodeDisposition::Retained);
    EXPECT_TRUE(hasBoundaryReason(propertyChangedChild->boundaryReasons,
                                  RenderBoundaryReason::Transform));
}

TEST(RetainedRenderTreeTest, OmitsInvisibleSubtrees) {
    Container root;
    auto hidden = std::make_unique<Container>();
    auto hiddenChild = std::make_unique<Container>();
    const uint64_t hiddenId = hidden->getObjectId();
    const uint64_t hiddenChildId = hiddenChild->getObjectId();
    hidden->addChild(std::move(hiddenChild));
    hidden->setVisible(false);
    root.addChild(std::move(hidden));

    const auto tree = RenderNodeCompiler{}.compile(root);

    ASSERT_EQ(tree.nodes.size(), 1u);
    EXPECT_EQ(tree.nodes[0].id, root.getObjectId());
    EXPECT_EQ(tree.find(hiddenId), nullptr);
    EXPECT_EQ(tree.find(hiddenChildId), nullptr);
    EXPECT_TRUE(tree.nodes[0].children.empty());
}

TEST(RetainedRenderTreeTest, TransactionFlattensOrdinaryWidgetsIntoRootContent) {
    Container root;
    auto child = std::make_unique<Container>();
    Container* childPtr = child.get();
    auto grandchild = std::make_unique<Container>();
    child->addChild(std::move(grandchild));
    root.addChild(std::move(child));

    const RenderNodeCompiler compiler;
    const RenderTreeDiffer differ;
    const auto initialTree = compiler.compile(root);
    const auto initial = differ.diff(nullptr, initialTree);

    EXPECT_TRUE(initial.replacesTree);
    ASSERT_EQ(initial.creates.size(), 1u);
    EXPECT_EQ(initial.creates[0].id, root.getObjectId());
    EXPECT_TRUE(initial.creates[0].children.empty());
    EXPECT_TRUE(initial.updates.empty());
    EXPECT_TRUE(initial.removals.empty());

    const auto unchangedTree = compiler.compile(root, initialTree.nodes.size());
    EXPECT_TRUE(differ.diff(&initialTree, unchangedTree).empty());

    childPtr->markDirty();
    const auto changedTree = compiler.compile(root, unchangedTree.nodes.size());
    const auto changed = differ.diff(&unchangedTree, changedTree);
    ASSERT_EQ(changed.updates.size(), 1u);
    EXPECT_EQ(changed.updates[0].node.id, root.getObjectId());
    EXPECT_TRUE(changed.updates[0].contentChanged);
    EXPECT_FALSE(changed.updates[0].propertiesChanged);
    EXPECT_TRUE(changed.creates.empty());
    EXPECT_TRUE(changed.removals.empty());
}

TEST(RetainedRenderTreeTest, TransactionPromotesAndDemotesPresentationBoundary) {
    Container root;
    auto child = std::make_unique<Container>();
    Container* childPtr = child.get();
    const uint64_t childId = child->getObjectId();
    root.addChild(std::move(child));

    const RenderNodeCompiler compiler;
    const RenderTreeDiffer differ;
    const auto flattenedTree = compiler.compile(root);

    childPtr->setTranslationX(5.0f);
    const auto promotedTree = compiler.compile(root, flattenedTree.nodes.size());
    const auto promoted = differ.diff(&flattenedTree, promotedTree);
    ASSERT_EQ(promoted.creates.size(), 1u);
    EXPECT_EQ(promoted.creates[0].id, childId);
    const auto* promotedRoot = findUpdate(promoted, root.getObjectId());
    ASSERT_NE(promotedRoot, nullptr);
    EXPECT_TRUE(promotedRoot->contentChanged);
    EXPECT_TRUE(promotedRoot->propertiesChanged);

    childPtr->setTranslationX(9.0f);
    const auto movedTree = compiler.compile(root, promotedTree.nodes.size());
    const auto moved = differ.diff(&promotedTree, movedTree);
    ASSERT_EQ(moved.updates.size(), 1u);
    EXPECT_EQ(moved.updates[0].node.id, childId);
    EXPECT_FALSE(moved.updates[0].contentChanged);
    EXPECT_TRUE(moved.updates[0].propertiesChanged);

    childPtr->setTranslationX(0.0f);
    const auto demotedTree = compiler.compile(root, movedTree.nodes.size());
    const auto demoted = differ.diff(&movedTree, demotedTree);
    ASSERT_EQ(demoted.removals.size(), 1u);
    EXPECT_EQ(demoted.removals[0], childId);
    const auto* demotedRoot = findUpdate(demoted, root.getObjectId());
    ASSERT_NE(demotedRoot, nullptr);
    EXPECT_TRUE(demotedRoot->contentChanged);
    EXPECT_TRUE(demotedRoot->propertiesChanged);
}

TEST(RetainedRenderTreeTest, TransactionKeepsScrollAndChildPaintUpdatesIsolated) {
    Container root;
    root.setWidth(100.0f);
    root.setHeight(100.0f);
    root.setDirection(lcl::ui::layout::Direction::Column);

    auto scroll = std::make_unique<ScrollView>();
    ScrollView* scrollPtr = scroll.get();
    scroll->setWidth(100.0f);
    scroll->setHeight(100.0f);
    const uint64_t scrollId = scroll->getObjectId();

    auto content = std::make_unique<Container>();
    Container* contentPtr = content.get();
    content->setWidth(100.0f);
    content->setHeight(300.0f);
    const uint64_t contentId = content->getObjectId();
    auto row = std::make_unique<Container>();
    Container* rowPtr = row.get();
    row->setWidth(100.0f);
    row->setHeight(40.0f);
    content->addChild(std::move(row));
    scroll->setContent(std::move(content));
    root.addChild(std::move(scroll));
    root.calculateLayout(100.0f, 100.0f);
    root.syncLayout();

    const RenderNodeCompiler compiler;
    const RenderTreeDiffer differ;
    const auto initialTree = compiler.compile(root);
    const auto initial = differ.diff(nullptr, initialTree);
    ASSERT_EQ(initial.creates.size(), 3u);
    EXPECT_EQ(initial.creates[0].id, root.getObjectId());
    EXPECT_EQ(initial.creates[1].id, scrollId);
    EXPECT_EQ(initial.creates[2].id, contentId);

    rowPtr->markDirty();
    const auto paintedTree = compiler.compile(root, initialTree.nodes.size());
    const auto painted = differ.diff(&initialTree, paintedTree);
    ASSERT_EQ(painted.updates.size(), 1u);
    EXPECT_EQ(painted.updates[0].node.id, contentId);
    EXPECT_TRUE(painted.updates[0].contentChanged);
    EXPECT_FALSE(painted.updates[0].propertiesChanged);

    const uint64_t scrollPaintRevision = scrollPtr->getPaintRevision();
    scrollPtr->setScrollY(40.0f);
    EXPECT_EQ(scrollPtr->getPaintRevision(), scrollPaintRevision);
    EXPECT_FLOAT_EQ(contentPtr->getPresentationState().translationY, -40.0f);
    const auto scrolledTree = compiler.compile(root, paintedTree.nodes.size());
    const auto scrolled = differ.diff(&paintedTree, scrolledTree);
    ASSERT_EQ(scrolled.updates.size(), 1u);
    EXPECT_EQ(scrolled.updates[0].node.id, contentId);
    EXPECT_FALSE(scrolled.updates[0].contentChanged);
    EXPECT_TRUE(scrolled.updates[0].propertiesChanged);
    EXPECT_TRUE(scrolled.creates.empty());
    EXPECT_TRUE(scrolled.removals.empty());
}

TEST(RetainedRenderTreeTest, TransactionRemovesNestedBoundariesChildFirst) {
    Container root;
    auto parent = std::make_unique<Container>();
    Container* parentPtr = parent.get();
    parent->setTranslationX(4.0f);
    const uint64_t parentId = parent->getObjectId();
    auto child = std::make_unique<Container>();
    Container* childPtr = child.get();
    child->setOpacity(0.5f);
    const uint64_t childId = child->getObjectId();
    parent->addChild(std::move(child));
    root.addChild(std::move(parent));

    const RenderNodeCompiler compiler;
    const RenderTreeDiffer differ;
    const auto retainedTree = compiler.compile(root);
    const auto initial = differ.diff(nullptr, retainedTree);
    ASSERT_EQ(initial.creates.size(), 3u);
    EXPECT_EQ(initial.creates[0].id, root.getObjectId());
    EXPECT_EQ(initial.creates[1].id, parentId);
    EXPECT_EQ(initial.creates[2].id, childId);

    parentPtr->setTranslationX(0.0f);
    childPtr->setOpacity(1.0f);
    const auto flattenedTree = compiler.compile(root, retainedTree.nodes.size());
    const auto removed = differ.diff(&retainedTree, flattenedTree);
    ASSERT_EQ(removed.removals.size(), 2u);
    EXPECT_EQ(removed.removals[0], childId);
    EXPECT_EQ(removed.removals[1], parentId);
}

} // namespace
