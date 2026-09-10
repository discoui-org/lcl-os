#pragma once

#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/navigation_bar.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lcl::ui {

struct NavigationRoute {
    std::string value;
    explicit NavigationRoute(std::string route) : value(std::move(route)) {}
    bool operator==(const NavigationRoute&) const = default;
};

struct NavigationPage {
    NavigationRoute route;
    std::string title;
    std::unique_ptr<Widget> content;
};

enum class PageTransition {
    None,
    Push,
    Pop,
    Replace,
};

/**
 * Persistent navigation chrome plus an animated page viewport.
 * Back navigation uses the NavigationBar control or a touch-only edge swipe.
 */
class NavigationStack final : public Container {
public:
    using RouteChangedCallback = std::function<void(const NavigationRoute&)>;

    NavigationStack();
    ~NavigationStack() override;

    bool setRootPage(NavigationPage page);
    bool push(NavigationPage page,
              PageTransition transition = PageTransition::Push);
    bool pop(PageTransition transition = PageTransition::Pop);
    bool replace(NavigationPage page,
                 PageTransition transition = PageTransition::Replace);
    /** Replaces the complete route history with one new root page. */
    bool reset(NavigationPage page,
               PageTransition transition = PageTransition::Replace);

    size_t pageCount() const noexcept { return m_entries.size(); }
    const NavigationRoute* currentRoute() const noexcept;
    bool isTransitioning() const noexcept { return m_transition.active; }
    NavigationBar& navigationBar() noexcept { return *m_navigationBar; }
    const NavigationBar& navigationBar() const noexcept {
        return *m_navigationBar;
    }
    void setOnRouteChanged(RouteChangedCallback callback) {
        m_onRouteChanged = std::move(callback);
    }
    /** Supplies a back destination owned by an enclosing split view. */
    void setParentBack(std::string title, std::function<void()> callback);
    void clearParentBack();

    void onPointerEventPreview(const PointerEvent& event) override;
    bool onPointerMove(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;

private:
    struct Entry {
        NavigationRoute route;
        std::string title;
        Widget* content{nullptr};
    };

    struct ActiveTransition {
        bool active{false};
        PageTransition kind{PageTransition::None};
        Widget* outgoing{nullptr};
        Widget* incoming{nullptr};
        Widget* activeAfterCompletion{nullptr};
        bool removeOutgoing{false};
        bool preparingPush{false};
        unsigned preparationTicks{0};
        float outgoingTarget{0.0f};
        float incomingTarget{0.0f};
        float preparedIncomingStart{0.0f};
        uint64_t generation{0};
    };

    enum class EdgeSwipeState { Idle, Pending, Dragging };

    struct EdgeSwipe {
        EdgeSwipeState state{EdgeSwipeState::Idle};
        uint32_t pointerId{0};
        float startX{0.0f};
        float startY{0.0f};
        float progress{0.0f};
        Widget* outgoing{nullptr};
        Widget* incoming{nullptr};
    };

    Widget* mountPage(NavigationPage& page);
    void updateBar();
    void notifyRouteChanged();
    void beginTransition(PageTransition kind, Widget* outgoing,
                         Widget* incoming);
    void beginSpringTransition(PageTransition kind, Widget* outgoing,
                               Widget* incoming, Widget* activeAfterCompletion,
                               bool removeOutgoing, float outgoingTarget,
                               float incomingTarget,
                               bool preparePush = false,
                               float preparedIncomingStart = 0.0f);
    void startSpringAnimations(uint64_t generation);
    void updateRetainedPageHints();
    void tickTransition(uint64_t generation);
    void completeTransition(uint64_t generation);
    void finishActiveTransition();
    void updateInteractivePop(float x);
    void finishInteractivePop(bool commit);
    void resetEdgeSwipe() noexcept;

    NavigationBar* m_navigationBar{nullptr};
    Container* m_viewport{nullptr};
    std::vector<Entry> m_entries;
    ActiveTransition m_transition;
    EdgeSwipe m_edgeSwipe;
    uint64_t m_nextTransitionGeneration{1};
    RouteChangedCallback m_onRouteChanged;
    std::string m_parentBackTitle;
    std::function<void()> m_onParentBack;
};

} // namespace lcl::ui
