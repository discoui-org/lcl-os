#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/divider.hpp"
#include "lcl-ui/widgets/icon.hpp"
#include "lcl-ui/widgets/navigation_split_view.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/text_field.hpp"
#include "system/render/raster_canvas.hpp"
#include "system/security/security_admin_client.hpp"
#include "system/security/session_user.hpp"
#include "system/security/sha256.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace layout = lcl::ui::layout;
using Color = lcl::graphics::Color;

constexpr float kInitialWidth = 960.0f;
constexpr float kInitialHeight = 680.0f;

constexpr Color kPageBackground{242, 242, 247, 255};
constexpr Color kSidebarBackground{247, 247, 250, 255};
constexpr Color kCardBackground{255, 255, 255, 255};
constexpr Color kPrimaryText{24, 25, 29, 255};
constexpr Color kSecondaryText{118, 120, 128, 255};
constexpr Color kSelection{220, 221, 226, 255};
constexpr Color kSeparator{198, 198, 203, 150};
constexpr Color kBlue{24, 126, 255, 255};
constexpr Color kGreen{49, 193, 103, 255};
constexpr Color kOrange{255, 145, 36, 255};
constexpr Color kGray{143, 146, 155, 255};
constexpr Color kPurple{154, 112, 205, 255};

constexpr std::string_view kGeneralRoute = "settings.general";
constexpr std::string_view kSecurityRoute = "settings.security";
constexpr std::string_view kReviewRoute = "settings.security.review";

bool takeSecurityCapability(lcl::security::SecurityAdminClient& client,
                            std::string& error) {
    const char* value = std::getenv("LCL_SECURITY_ADMIN_FD");
    if (!value || std::string_view(value) != "4") {
        error = "Settings was not launched with its security capability";
        return false;
    }
    unsetenv("LCL_SECURITY_ADMIN_FD");
    return client.adoptCapabilityDescriptor(4, error);
}

class SettingsApp final {
public:
    SettingsApp(lcl::ui::WindowApp& window,
                lcl::security::SecurityAdminClient& security)
        : window_(window), security_(security) {}

    void refresh() {
        std::string error;
        std::vector<lcl::security::PendingBundleApproval> pending;
        if (security_.pendingBundleApprovals(pending, error)) {
            pending_ = std::move(pending);
            serviceMessage_.clear();
        } else {
            pending_.clear();
            serviceMessage_ = "Security service error: " + error;
        }
        if (split_) reloadDetail();
        else rebuild();
    }

    void rebuild() {
        const auto& environment = window_.getLayoutEnvironment();
        expanded_ = environment.sizeClass == lcl::ui::LayoutSizeClass::Expanded;

        auto root = std::make_unique<lcl::ui::Container>();
        root_ = root.get();
        root->setDirection(layout::Direction::Column);
        root->setBackgroundColor(kPageBackground);
        applySafeArea(environment.safeArea);

        auto split = std::make_unique<lcl::ui::NavigationSplitView>();
        split_ = split.get();
        split->setFlexGrow(1.0f);
        split->setPrimaryTitle("Settings");
        split->setSidebarWidth(292.0f);
        split->setSizeClass(environment.sizeClass);
        split->setSidebar(makeSidebar());

        auto& navigation = split->detailNavigation();
        const Route detailRoot = route_ == Route::Review
            ? Route::Security : route_;
        navigation.setRootPage(makePage(detailRoot));
        if (route_ == Route::Review && confirmation_) {
            navigation.push(makePage(Route::Review), lcl::ui::PageTransition::None);
        }
        navigation.setOnRouteChanged([this](const lcl::ui::NavigationRoute& route) {
            if (route.value == kGeneralRoute) {
                route_ = Route::General;
                confirmation_.reset();
            } else if (route.value == kSecurityRoute) {
                route_ = Route::Security;
                confirmation_.reset();
            } else if (route.value == kReviewRoute) {
                route_ = Route::Review;
            }
            updateSidebarSelection();
        });
        root->addChild(std::move(split));
        window_.setRootWidget(std::move(root));
        updateSidebarSelection();
    }

    void updateLayoutEnvironment(const lcl::ui::LayoutEnvironment& environment) {
        expanded_ = environment.sizeClass == lcl::ui::LayoutSizeClass::Expanded;
        applySafeArea(environment.safeArea);
        if (split_) split_->setSizeClass(environment.sizeClass);
    }

private:
    enum class Route { General, Security, Review };

    std::unique_ptr<lcl::ui::Text> text(const std::string& value, float size,
                                        Color color = kPrimaryText) const {
        auto result = std::make_unique<lcl::ui::Text>(value);
        result->setFontSize(size);
        result->setTextColor(color);
        return result;
    }

    std::unique_ptr<lcl::ui::Container> card(float radius = 18.0f) const {
        auto result = std::make_unique<lcl::ui::Container>();
        result->setDirection(layout::Direction::Column);
        result->setBackgroundColor(kCardBackground);
        result->setBorderRadius(radius);
        return result;
    }

    std::unique_ptr<lcl::ui::Divider> divider() const {
        auto result = std::make_unique<lcl::ui::Divider>();
        result->setColor(kSeparator);
        return result;
    }

    std::unique_ptr<lcl::ui::Container> iconTile(lcl::ui::IconData symbol,
                                                  Color color) const {
        auto tile = std::make_unique<lcl::ui::Container>();
        tile->setWidth(30.0f);
        tile->setHeight(30.0f);
        tile->setDirection(layout::Direction::Column);
        tile->setJustifyContent(layout::Justify::Center);
        tile->setAlignItems(layout::Align::Center);
        tile->setBackgroundColor(color);
        tile->setBorderRadius(8.0f);
        auto icon = std::make_unique<lcl::ui::Icon>(symbol);
        icon->setIconSize(18.0f);
        icon->setColor({255, 255, 255, 255});
        tile->addChild(std::move(icon));
        return tile;
    }

    std::unique_ptr<lcl::ui::Container> disclosureRow(
            lcl::ui::IconData symbol, Color color, const std::string& title,
            const std::string& value, bool selected,
            std::function<void()> action) const {
        auto row = std::make_unique<lcl::ui::Container>();
        row->setDirection(layout::Direction::Row);
        row->setAlignItems(layout::Align::Center);
        row->setJustifyContent(layout::Justify::SpaceBetween);
        row->setHeight(50.0f);
        row->setPadding(layout::Edge::Horizontal, 11.0f);
        row->setBackgroundColor(selected ? kSelection : Color{0, 0, 0, 0});
        row->setBorderRadius(11.0f);
        row->setFocusable(true);
        row->setInteractionStyle(lcl::ui::InteractionState::Hover,
                                 lcl::ui::InteractionStyle{
                                     .scale = std::nullopt,
                                     .opacity = 0.82f,
                                     .motion = std::nullopt,
                                 });
        row->setInteractionStyle(lcl::ui::InteractionState::Pressed,
                                 lcl::ui::InteractionStyle{
                                     .scale = 0.985f,
                                     .opacity = std::nullopt,
                                     .motion = std::nullopt,
                                 });
        if (action) row->setOnClick(std::move(action));

        auto leading = std::make_unique<lcl::ui::Container>();
        leading->setDirection(layout::Direction::Row);
        leading->setAlignItems(layout::Align::Center);
        leading->setGap(11.0f);
        leading->addChild(iconTile(symbol, color));
        leading->addChild(text(title, 14.0f));
        row->addChild(std::move(leading));

        auto trailing = std::make_unique<lcl::ui::Container>();
        trailing->setDirection(layout::Direction::Row);
        trailing->setAlignItems(layout::Align::Center);
        trailing->setGap(8.0f);
        if (!value.empty()) trailing->addChild(text(value, 13.0f, kSecondaryText));
        auto chevron = std::make_unique<lcl::ui::Icon>(lcl::ui::icons::ChevronRight);
        chevron->setIconSize(14.0f);
        chevron->setColor({180, 182, 188, 255});
        trailing->addChild(std::move(chevron));
        row->addChild(std::move(trailing));
        return row;
    }

    std::unique_ptr<lcl::ui::Container> accountCard(bool compact) const {
        auto account = card(compact ? 22.0f : 15.0f);
        account->setDirection(layout::Direction::Row);
        account->setAlignItems(layout::Align::Center);
        account->setPadding(compact ? 16.0f : 11.0f);
        account->setGap(12.0f);

        auto avatar = std::make_unique<lcl::ui::Container>();
        avatar->setWidth(compact ? 52.0f : 40.0f);
        avatar->setHeight(compact ? 52.0f : 40.0f);
        avatar->setDirection(layout::Direction::Column);
        avatar->setJustifyContent(layout::Justify::Center);
        avatar->setAlignItems(layout::Align::Center);
        avatar->setBackgroundColor(kPurple);
        avatar->setBorderRadius(compact ? 26.0f : 20.0f);
        auto person = std::make_unique<lcl::ui::Icon>(lcl::ui::icons::PersonCircle);
        person->setIconSize(compact ? 31.0f : 25.0f);
        person->setColor({255, 255, 255, 255});
        avatar->addChild(std::move(person));
        account->addChild(std::move(avatar));

        auto labels = std::make_unique<lcl::ui::Container>();
        labels->setDirection(layout::Direction::Column);
        labels->setGap(2.0f);
        labels->setFlexGrow(1.0f);
        labels->addChild(text("Rei", compact ? 18.0f : 14.0f));
        labels->addChild(text("Account, device and services", compact ? 13.0f : 11.0f,
                              kSecondaryText));
        account->addChild(std::move(labels));
        auto chevron = std::make_unique<lcl::ui::Icon>(lcl::ui::icons::ChevronRight);
        chevron->setIconSize(compact ? 16.0f : 13.0f);
        chevron->setColor({180, 182, 188, 255});
        account->addChild(std::move(chevron));
        return account;
    }

    std::unique_ptr<lcl::ui::Widget> makeSearchField() const {
        auto search = std::make_unique<lcl::ui::Container>();
        search->setDirection(layout::Direction::Row);
        search->setAlignItems(layout::Align::Center);
        search->setHeight(38.0f);
        search->setGap(7.0f);
        search->setPadding(layout::Edge::Horizontal, 11.0f);
        search->setBackgroundColor({232, 232, 237, 255});
        search->setBorderRadius(11.0f);
        auto icon = std::make_unique<lcl::ui::Icon>(lcl::ui::icons::Search);
        icon->setIconSize(15.0f);
        icon->setColor(kSecondaryText);
        search->addChild(std::move(icon));
        auto field = std::make_unique<lcl::ui::TextField>();
        field->setPlaceholder("Search");
        field->setFlexGrow(1.0f);
        field->setHeight(36.0f);
        lcl::theme::WidgetStyle searchStyle;
        searchStyle.normal.background = Color{0, 0, 0, 0};
        searchStyle.normal.foreground = kPrimaryText;
        searchStyle.normal.secondaryForeground = kSecondaryText;
        searchStyle.normal.accent = kBlue;
        searchStyle.normal.border = Color{0, 0, 0, 0};
        searchStyle.normal.borderWidth = 0.0f;
        searchStyle.normal.cornerRadius = 0.0f;
        searchStyle.focused = searchStyle.normal;
        searchStyle.horizontalPadding = 0.0f;
        field->useStyle(std::move(searchStyle));
        search->addChild(std::move(field));
        return search;
    }

    std::unique_ptr<lcl::ui::Widget> makeSidebar() {
        auto sidebar = std::make_unique<lcl::ui::Container>();
        sidebar->setDirection(layout::Direction::Column);
        sidebar->setGap(9.0f);
        sidebar->setPadding(15.0f);
        sidebar->setBackgroundColor(kSidebarBackground);
        sidebar->addChild(makeSearchField());
        sidebar->addChild(accountCard(false));
        auto general = disclosureRow(
            lcl::ui::icons::Gear, kGray, "General", "",
            route_ == Route::General,
            [this] { selectPrimaryRoute(Route::General); });
        generalRow_ = general.get();
        sidebar->addChild(std::move(general));
        auto security = disclosureRow(
            lcl::ui::icons::ShieldFill, kBlue, "Security", "",
            route_ == Route::Security || route_ == Route::Review,
            [this] { selectPrimaryRoute(Route::Security); });
        securityRow_ = security.get();
        sidebar->addChild(std::move(security));
        sidebar->addChild(disclosureRow(
            lcl::ui::icons::Paintbrush, kPurple, "Appearance", "", false,
            [] {}));
        sidebar->addChild(disclosureRow(
            lcl::ui::icons::PersonCircle, kGreen, "Accessibility", "", false,
            [] {}));
        return sidebar;
    }

    lcl::ui::NavigationPage makePage(Route route) {
        std::string routeId;
        std::string title;
        if (route == Route::General) {
            routeId = std::string(kGeneralRoute);
            title = "General";
        } else if (route == Route::Security) {
            routeId = std::string(kSecurityRoute);
            title = "Security";
        } else {
            routeId = std::string(kReviewRoute);
            title = "Review Bundle";
        }
        return {
            .route = lcl::ui::NavigationRoute(std::move(routeId)),
            .title = std::move(title),
            .content = makePageContent(route),
        };
    }

    std::unique_ptr<lcl::ui::Widget> makePageContent(Route route) {
        auto content = std::make_unique<lcl::ui::Container>();
        content->setDirection(layout::Direction::Column);
        content->setPadding(expanded_ ? 28.0f : 18.0f);
        content->setGap(expanded_ ? 18.0f : 15.0f);
        content->setBackgroundColor(kPageBackground);
        if (route == Route::General) appendGeneral(*content);
        else if (route == Route::Security) appendSecurity(*content);
        else appendReview(*content);
        return content;
    }

    void appendGeneral(lcl::ui::Container& content) {
        auto hero = card();
        hero->setPadding(20.0f);
        hero->setGap(8.0f);
        auto heading = std::make_unique<lcl::ui::Container>();
        heading->setDirection(layout::Direction::Row);
        heading->setAlignItems(layout::Align::Center);
        heading->setGap(12.0f);
        heading->addChild(iconTile(lcl::ui::icons::Gear, kGray));
        heading->addChild(text("General", 20.0f));
        hero->addChild(std::move(heading));
        hero->addChild(text(
            "Manage device information, software updates, storage, language, and time.",
            13.0f, kSecondaryText));
        content.addChild(std::move(hero));

        auto device = card();
        device->setPadding(layout::Edge::Horizontal, 14.0f);
        device->addChild(disclosureRow(lcl::ui::icons::InfoCircle, kGray,
                                        "About", "", false, [] {}));
        device->addChild(divider());
        device->addChild(disclosureRow(lcl::ui::icons::Gear, kBlue,
                                        "Software Update", "", false, [] {}));
        device->addChild(divider());
        device->addChild(disclosureRow(lcl::ui::icons::Shield, kGray,
                                        "Device Storage", "", false, [] {}));
        content.addChild(std::move(device));

        auto regional = card();
        regional->setPadding(layout::Edge::Horizontal, 14.0f);
        regional->addChild(disclosureRow(lcl::ui::icons::Gear, kGray,
                                          "Date & Time", "", false, [] {}));
        regional->addChild(divider());
        regional->addChild(disclosureRow(lcl::ui::icons::InfoCircle, kBlue,
                                          "Language & Region", "", false, [] {}));
        content.addChild(std::move(regional));
    }

    void appendSecurity(lcl::ui::Container& content) {
        auto hero = card();
        hero->setPadding(19.0f);
        hero->setGap(7.0f);
        auto heading = std::make_unique<lcl::ui::Container>();
        heading->setDirection(layout::Direction::Row);
        heading->setAlignItems(layout::Align::Center);
        heading->setGap(11.0f);
        heading->addChild(iconTile(lcl::ui::icons::ShieldFill, kBlue));
        heading->addChild(text("Bundle approvals", 19.0f));
        hero->addChild(std::move(heading));
        hero->addChild(text("Review unverified publishers before their software can run.",
                            13.0f, kSecondaryText));
        content.addChild(std::move(hero));

        auto pendingCard = card();
        pendingCard->setPadding(layout::Edge::Horizontal, 14.0f);
        pendingCard->setPadding(layout::Edge::Vertical, 7.0f);
        if (!serviceMessage_.empty()) {
            pendingCard->setPadding(16.0f);
            pendingCard->addChild(text(serviceMessage_, 14.0f,
                                       {198, 55, 55, 255}));
        } else if (pending_.empty()) {
            pendingCard->setPadding(17.0f);
            pendingCard->setGap(5.0f);
            pendingCard->addChild(text("No pending approvals", 16.0f));
            pendingCard->addChild(text("New unverified bundles will appear here.",
                                       13.0f, kSecondaryText));
        } else {
            for (size_t index = 0; index < pending_.size(); ++index) {
                const auto request = pending_[index];
                pendingCard->addChild(disclosureRow(
                    lcl::ui::icons::Shield, kOrange, request.displayName,
                    lcl::security::hexEncodeDigest(request.bundleRecordDigest).substr(0, 10),
                    false, [this, request] { navigateToReview(request); }));
                if (index + 1 < pending_.size())
                    pendingCard->addChild(divider());
            }
        }
        content.addChild(std::move(pendingCard));

        auto refreshButton = std::make_unique<lcl::ui::Button>("Refresh");
        refreshButton->setWidth(116.0f);
        refreshButton->setHeight(38.0f);
        refreshButton->setOnClick([this] { refresh(); });
        content.addChild(std::move(refreshButton));
    }

    void appendReview(lcl::ui::Container& content) {
        if (!confirmation_) return;
        const auto& request = *confirmation_;
        auto detail = card();
        detail->setPadding(19.0f);
        detail->setGap(8.0f);
        detail->addChild(text(request.displayName, 19.0f));
        detail->addChild(text(request.appId, 13.0f, kSecondaryText));
        detail->addChild(text(request.bundlePath, 12.0f, kSecondaryText));
        detail->addChild(divider());
        detail->addChild(text("Publisher: unverified", 14.0f));
        detail->addChild(text("Exact bundle record", 12.0f, kSecondaryText));
        detail->addChild(text(lcl::security::hexEncodeDigest(request.bundleRecordDigest),
                              12.0f, kSecondaryText));
        detail->addChild(text("Allows only this record in its normal sandbox.",
                              13.0f, kSecondaryText));
        detail->addChild(text("No signature, root access, or elevation is granted.",
                              13.0f, kSecondaryText));
        content.addChild(std::move(detail));

        auto allow = std::make_unique<lcl::ui::Button>("Allow in Sandbox");
        allow->setHeight(42.0f);
        allow->setMaxWidth(320.0f);
        allow->setBackgroundColor(kBlue);
        allow->setBorderRadius(12.0f);
        allow->getTextWidget()->setTextColor({255, 255, 255, 255});
        allow->setOnClick([this] { approveSelected(); });
        content.addChild(std::move(allow));
    }

    void navigateToReview(lcl::security::PendingBundleApproval request) {
        if (!split_) return;
        confirmation_ = std::move(request);
        route_ = Route::Review;
        updateSidebarSelection();
        split_->detailNavigation().push(
            makePage(Route::Review), lcl::ui::PageTransition::Push);
    }

    void selectPrimaryRoute(Route route) {
        if (!split_) return;
        if (route_ == route) {
            split_->presentDetail();
            return;
        }
        route_ = route;
        confirmation_.reset();
        updateSidebarSelection();
        split_->setDetailPage(
            makePage(route), lcl::ui::PageTransition::Replace);
    }

    void reloadDetail() {
        if (!split_) return;
        const Route detailRoot = route_ == Route::Review
            ? Route::Security : route_;
        split_->detailNavigation().reset(
            makePage(detailRoot), lcl::ui::PageTransition::None);
        if (route_ == Route::Review && confirmation_) {
            split_->detailNavigation().push(
                makePage(Route::Review), lcl::ui::PageTransition::None);
        }
        updateSidebarSelection();
    }

    void updateSidebarSelection() {
        if (generalRow_) {
            generalRow_->setBackgroundColor(
                route_ == Route::General ? kSelection : Color{0, 0, 0, 0});
        }
        if (securityRow_) {
            securityRow_->setBackgroundColor(
                route_ == Route::Security || route_ == Route::Review
                    ? kSelection : Color{0, 0, 0, 0});
        }
    }

    void applySafeArea(const lcl::ui::LayoutInsets& safeArea) {
        if (!root_) return;
        const float topInset = expanded_
            ? safeArea.top : std::max(safeArea.top, 24.0f);
        const float bottomInset = expanded_
            ? safeArea.bottom : std::max(safeArea.bottom, 16.0f);
        root_->setPadding(layout::Edge::Top, topInset);
        root_->setPadding(layout::Edge::Right, safeArea.right);
        root_->setPadding(layout::Edge::Bottom, bottomInset);
        root_->setPadding(layout::Edge::Left, safeArea.left);
    }

    void approveSelected() {
        if (!confirmation_) return;
        const lcl::security::SecurityAdminBundleIdentity identity{
            .appId = confirmation_->appId,
            .bundleRecordDigest = confirmation_->bundleRecordDigest,
        };
        const std::string appId = identity.appId;
        std::string error;
        if (security_.approveBundle(identity, error)) {
            serviceMessage_ = "Allowed " + appId + " for its exact bundle record.";
            std::vector<lcl::security::PendingBundleApproval> refreshed;
            if (security_.pendingBundleApprovals(refreshed, error)) {
                pending_ = std::move(refreshed);
            } else {
                serviceMessage_ = "Bundle allowed; refresh failed: " + error;
            }
        } else {
            serviceMessage_ = "Allow was rejected: " + error;
        }
        confirmation_.reset();
        route_ = Route::Security;
        reloadDetail();
    }

    lcl::ui::WindowApp& window_;
    lcl::security::SecurityAdminClient& security_;
    lcl::ui::Container* root_{nullptr};
    lcl::ui::NavigationSplitView* split_{nullptr};
    lcl::ui::Container* generalRow_{nullptr};
    lcl::ui::Container* securityRow_{nullptr};
    Route route_{Route::General};
    bool expanded_{false};
    std::vector<lcl::security::PendingBundleApproval> pending_;
    std::optional<lcl::security::PendingBundleApproval> confirmation_;
    std::string serviceMessage_;
};

} // namespace

int main() {
    lcl::security::SecurityAdminClient security;
    std::string error;
    if (!takeSecurityCapability(security, error)) {
        std::cerr << "[LCL Settings ERROR] " << error << '\n';
        return 1;
    }

    lcl::ui::WindowApp window(lcl::render::makeDisplayListCanvas(),
                              kInitialWidth, kInitialHeight, "Settings");
    window.setAppId(lcl::security::kSystemSettingsAppId);
    window.setInitialBounds(100.0f, 80.0f, kInitialWidth, kInitialHeight);
    window.setDecorationMode(lcl::protocol::LCLDecorationMode::SSD);
    window.setWindowCornerStyle(18.0f, 2.0f);

    SettingsApp settings(window, security);
    window.setOnLayoutEnvironmentChanged(
        [&settings](const lcl::ui::LayoutEnvironment& environment) {
            settings.updateLayoutEnvironment(environment);
        });
    settings.refresh();
    if (!window.connectCompositor()) return 1;
    window.runEventLoop();
    return 0;
}
