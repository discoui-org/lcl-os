#include "lcl-ui/widgets/navigation_bar.hpp"

#include "lcl-ui/widgets/divider.hpp"
#include "lcl-ui/widgets/icon.hpp"
#include "lcl-ui/widgets/text.hpp"

namespace lcl::ui {
namespace {

constexpr graphics::Color kBarBackground{248, 248, 250, 255};
constexpr graphics::Color kTitleColor{23, 23, 26, 255};
constexpr graphics::Color kActionColor{25, 122, 255, 255};
constexpr graphics::Color kSeparatorColor{198, 198, 203, 150};

} // namespace

NavigationBar::NavigationBar() {
    setDirection(layout::Direction::Column);
    setHeight(56.0f);
    setBackgroundColor(kBarBackground);

    auto content = std::make_unique<Container>();
    content->setDirection(layout::Direction::Row);
    content->setAlignItems(layout::Align::Center);
    content->setHeight(55.0f);
    content->setPadding(layout::Edge::Horizontal, 10.0f);

    auto back = std::make_unique<Container>();
    m_backControl = back.get();
    back->setWidth(128.0f);
    back->setHeight(44.0f);
    back->setDirection(layout::Direction::Row);
    back->setAlignItems(layout::Align::Center);
    back->setGap(3.0f);
    back->setFocusable(true);
    back->setInteractionStyle(InteractionState::Pressed,
                              InteractionStyle{
                                  .scale = std::nullopt,
                                  .opacity = 0.45f,
                                  .motion = std::nullopt,
                              });
    back->setOnClick([this] {
        if (m_canGoBack && m_onBack) m_onBack();
    });
    auto backIcon = std::make_unique<Icon>(icons::Back);
    backIcon->setIconSize(21.0f);
    backIcon->setColor(kActionColor);
    back->addChild(std::move(backIcon));
    auto backLabel = std::make_unique<Text>();
    m_backLabel = backLabel.get();
    backLabel->setFontSize(15.0f);
    backLabel->setTextColor(kActionColor);
    back->addChild(std::move(backLabel));
    content->addChild(std::move(back));

    auto title = std::make_unique<Text>();
    m_titleLabel = title.get();
    title->setFlexGrow(1.0f);
    title->setTextAlign(TextAlign::Center);
    title->setFontSize(17.0f);
    title->setTextColor(kTitleColor);
    content->addChild(std::move(title));

    auto trailingBalance = std::make_unique<Container>();
    trailingBalance->setWidth(128.0f);
    trailingBalance->setHeight(44.0f);
    content->addChild(std::move(trailingBalance));
    addChild(std::move(content));

    auto divider = std::make_unique<Divider>();
    divider->setHeight(1.0f);
    divider->setColor(kSeparatorColor);
    addChild(std::move(divider));
    setCanGoBack(false);
}

void NavigationBar::setTitle(std::string title) {
    if (m_title == title) return;
    m_title = std::move(title);
    if (m_titleLabel) m_titleLabel->setText(m_title);
}

void NavigationBar::setBackTitle(std::string title) {
    if (m_backTitle == title) return;
    m_backTitle = std::move(title);
    if (m_backLabel) m_backLabel->setText(m_backTitle);
}

void NavigationBar::setCanGoBack(bool canGoBack) {
    if (m_canGoBack == canGoBack && m_backControl) return;
    m_canGoBack = canGoBack;
    if (!m_backControl) return;
    // Paint-only hiding preserves the leading width so the page title remains
    // centered when the back button appears or disappears.
    m_backControl->setVisible(canGoBack);
    m_backControl->setInteractionEnabled(canGoBack);
}

} // namespace lcl::ui
