#pragma once

#include "lcl-ui/core/layout_environment.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/navigation_stack.hpp"

#include <memory>
#include <string>

namespace lcl::ui {

class NavigationBar;

/**
 * One adaptive primary/detail navigation owner.
 *
 * Expanded layouts keep both panes visible. Compact layouts retain the same
 * pane instances and present detail over primary with an interactive edge-pop.
 */
class NavigationSplitView final : public Container {
public:
    NavigationSplitView();
    ~NavigationSplitView() override;

    void setSidebar(std::unique_ptr<Widget> sidebar);
    void setSidebarWidth(float width);
    float sidebarWidth() const noexcept { return m_sidebarWidth; }
    void setPrimaryTitle(std::string title);
    void setSizeClass(LayoutSizeClass sizeClass);
    LayoutSizeClass sizeClass() const noexcept { return m_sizeClass; }

    NavigationStack& detailNavigation() noexcept { return *m_detail; }
    const NavigationStack& detailNavigation() const noexcept { return *m_detail; }

    bool setDetailPage(NavigationPage page,
                       PageTransition transition = PageTransition::Replace);
    void presentDetail();
    void presentPrimary();
    bool isDetailPresented() const noexcept {
        return m_sizeClass == LayoutSizeClass::Expanded ||
            m_compactDetailPresented;
    }
    bool isTransitioning() const noexcept { return m_transitionActive; }

    void syncLayout(float parentAbsX = 0.0f,
                    float parentAbsY = 0.0f) override;
    void onPointerEventPreview(const PointerEvent& event) override;
    bool onPointerMove(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;

private:
    enum class EdgeState { Idle, Pending, Dragging };

    void applyMode(bool preservePresentation = false);
    void animateCompactPresentation(bool detailPresented);
    void tickTransition();
    void finishTransition();
    void updateEdgeDrag(float x);
    void finishEdgeDrag(bool commit);
    void resetEdgeDrag() noexcept;

    Container* m_sidebarPane{nullptr};
    NavigationBar* m_primaryBar{nullptr};
    Container* m_sidebarHost{nullptr};
    NavigationStack* m_detail{nullptr};
    float m_sidebarWidth{292.0f};
    LayoutSizeClass m_sizeClass{LayoutSizeClass::Compact};
    bool m_compactDetailPresented{false};
    bool m_transitionActive{false};
    bool m_transitionTargetDetail{false};
    EdgeState m_edgeState{EdgeState::Idle};
    uint32_t m_edgePointerId{0};
    float m_edgeStartX{0.0f};
    float m_edgeStartY{0.0f};
    float m_edgeProgress{0.0f};
};

} // namespace lcl::ui
