#include "lcl-ui/layout/yoga_node.hpp"
#include <utility>

namespace lcl::ui {

YogaNode::YogaNode() : m_node(YGNodeNew()) {
    YGNodeSetContext(m_node, this);
}

YogaNode::YogaNode(YGNodeRef node) : m_node(node) {
    if (m_node) {
        YGNodeSetContext(m_node, this);
    }
}

YogaNode::~YogaNode() {
    if (m_node) {
        YGNodeFree(m_node);
        m_node = nullptr;
    }
}

YogaNode::YogaNode(YogaNode&& other) noexcept
    : m_node(std::exchange(other.m_node, nullptr)),
      m_measureCallback(std::move(other.m_measureCallback)) {
    if (m_node) {
        YGNodeSetContext(m_node, this);
    }
}

YogaNode& YogaNode::operator=(YogaNode&& other) noexcept {
    if (this != &other) {
        if (m_node) {
            YGNodeFree(m_node);
        }
        m_node = std::exchange(other.m_node, nullptr);
        m_measureCallback = std::move(other.m_measureCallback);
        if (m_node) {
            YGNodeSetContext(m_node, this);
        }
    }
    return *this;
}

void YogaNode::insertChild(YogaNode* child, uint32_t index) {
    if (m_node && child && child->m_node) {
        YGNodeInsertChild(m_node, child->m_node, index);
    }
}

void YogaNode::appendChild(YogaNode* child) {
    if (m_node && child && child->m_node) {
        uint32_t childCount = YGNodeGetChildCount(m_node);
        YGNodeInsertChild(m_node, child->m_node, childCount);
    }
}

void YogaNode::removeChild(YogaNode* child) {
    if (m_node && child && child->m_node) {
        YGNodeRemoveChild(m_node, child->m_node);
    }
}

void YogaNode::removeAllChildren() {
    if (m_node) {
        YGNodeRemoveAllChildren(m_node);
    }
}

void YogaNode::calculateLayout(float parentWidth, float parentHeight, YGDirection direction) {
    if (m_node) {
        YGNodeCalculateLayout(m_node, parentWidth, parentHeight, direction);
    }
}

YGSize YogaNode::staticMeasureFunc(YGNodeRef node, float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode) {
    if (node) {
        auto* self = static_cast<YogaNode*>(YGNodeGetContext(node));
        if (self && self->m_measureCallback) {
            return self->m_measureCallback(width, widthMode, height, heightMode);
        }
    }
    return YGSize{0.0f, 0.0f};
}

void YogaNode::setMeasureFunc(MeasureCallback callback) {
    m_measureCallback = callback;
    if (m_node) {
        YGNodeSetMeasureFunc(m_node, callback ? &YogaNode::staticMeasureFunc : nullptr);
    }
}

void YogaNode::markDirty() {
    if (m_node) YGNodeMarkDirty(m_node);
}

} // namespace lcl::ui
