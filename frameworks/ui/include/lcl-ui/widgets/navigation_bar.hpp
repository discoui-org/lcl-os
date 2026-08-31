#pragma once

#include "lcl-ui/widgets/container.hpp"

#include <functional>
#include <string>

namespace lcl::ui {

class Text;

/** Persistent page chrome with a centered title and explicit back control. */
class NavigationBar final : public Container {
public:
    NavigationBar();

    void setTitle(std::string title);
    const std::string& title() const noexcept { return m_title; }
    void setBackTitle(std::string title);
    const std::string& backTitle() const noexcept { return m_backTitle; }
    void setCanGoBack(bool canGoBack);
    bool canGoBack() const noexcept { return m_canGoBack; }
    void setOnBack(std::function<void()> callback) {
        m_onBack = std::move(callback);
    }

private:
    Container* m_backControl{nullptr};
    Text* m_backLabel{nullptr};
    Text* m_titleLabel{nullptr};
    std::string m_title;
    std::string m_backTitle;
    bool m_canGoBack{false};
    std::function<void()> m_onBack;
};

} // namespace lcl::ui
