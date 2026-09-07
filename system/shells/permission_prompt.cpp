#include "system/shells/permission_prompt.hpp"

#include <algorithm>
#include <utility>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "system/ipc/lcl_protocol.hpp"
#include "system/render/raster_canvas.hpp"

namespace lcl::shell {
namespace {

namespace layout = lcl::ui::layout;

void absolute(lcl::ui::Widget& widget, float x, float y, float width,
              float height) {
    widget.setPositionType(layout::PositionType::Absolute);
    widget.setPosition(layout::Edge::Left, x);
    widget.setPosition(layout::Edge::Top, y);
    widget.setWidth(width);
    widget.setHeight(height);
}

} // namespace

PermissionPromptLayout layoutPermissionPrompt(
        PermissionPromptPresentation presentation, float outputWidth,
        float outputHeight) noexcept {
    outputWidth = std::max(1.0f, outputWidth);
    outputHeight = std::max(1.0f, outputHeight);
    if (presentation == PermissionPromptPresentation::Mobile) {
        const float height = std::min(278.0f, outputHeight);
        return {0.0f, outputHeight - height, outputWidth, height, true};
    }

    const float width = std::min(440.0f, std::max(1.0f, outputWidth - 32.0f));
    const float height = std::min(244.0f, std::max(1.0f, outputHeight - 32.0f));
    return {(outputWidth - width) * 0.5f, (outputHeight - height) * 0.5f,
            width, height, false};
}

PermissionPromptSurface::PermissionPromptSurface(
        PermissionPromptPresentation presentation, std::uint32_t surfaceId,
        std::string shellAppId, std::uint32_t width, std::uint32_t height)
    : presentation_(presentation), surfaceId_(surfaceId),
      shellAppId_(std::move(shellAppId)), width_(width), height_(height) {}

PermissionPromptSurface::~PermissionPromptSurface() = default;

std::unique_ptr<lcl::ui::Container> PermissionPromptSurface::makeRoot(
        std::uint32_t width, std::uint32_t height) {
    const auto prompt = layoutPermissionPrompt(
        presentation_, static_cast<float>(width), static_cast<float>(height));

    auto root = std::make_unique<lcl::ui::Container>();
    root->setWidth(static_cast<float>(width));
    root->setHeight(static_cast<float>(height));
    root->setBackgroundColor({0, 0, 0, 92});

    auto card = std::make_unique<lcl::ui::Container>();
    card->setBackgroundColor({28, 28, 30, 250});
    card->setBorderColor({255, 255, 255, 38});
    card->setBorderWidth(1.0f);
    card->setBorderRadius(
        presentation_ == PermissionPromptPresentation::Mobile ? 24.0f : 18.0f);
    card->setTopOnlyBorderRadius(prompt.topCornersOnly);
    absolute(*card, prompt.x, prompt.y, prompt.width, prompt.height);

    const float horizontalPadding =
        presentation_ == PermissionPromptPresentation::Mobile ? 24.0f : 28.0f;
    const float contentWidth = std::max(1.0f, prompt.width - horizontalPadding * 2.0f);

    auto appName = std::make_unique<lcl::ui::Text>(content_.appName);
    appName->setFontSize(13.0f);
    appName->setTextColor({174, 174, 178, 255});
    absolute(*appName, horizontalPadding, 24.0f, contentWidth, 20.0f);
    card->addChild(std::move(appName));

    auto title = std::make_unique<lcl::ui::Text>(content_.title);
    title->setFontSize(21.0f);
    title->setTextColor({250, 250, 250, 255});
    absolute(*title, horizontalPadding, 51.0f, contentWidth, 32.0f);
    card->addChild(std::move(title));

    auto description = std::make_unique<lcl::ui::Text>(content_.description);
    description->setFontSize(14.0f);
    description->setTextColor({199, 199, 204, 255});
    absolute(*description, horizontalPadding, 91.0f, contentWidth, 56.0f);
    card->addChild(std::move(description));

    constexpr float buttonHeight = 44.0f;
    constexpr float buttonGap = 10.0f;
    const float buttonY = prompt.height - buttonHeight - 22.0f;
    const float buttonWidth = (contentWidth - buttonGap) * 0.5f;

    auto deny = std::make_unique<lcl::ui::Button>("Don't Allow");
    deny->setBackgroundColor({58, 58, 60, 255});
    deny->setBorderRadius(11.0f);
    deny->getTextWidget()->setTextColor({250, 250, 250, 255});
    deny->setOnClick([this] { queueDecision(false); });
    absolute(*deny, horizontalPadding, buttonY, buttonWidth, buttonHeight);
    card->addChild(std::move(deny));

    auto allow = std::make_unique<lcl::ui::Button>("Allow");
    allow->setBackgroundColor({10, 132, 255, 255});
    allow->setBorderRadius(11.0f);
    allow->getTextWidget()->setTextColor({255, 255, 255, 255});
    allow->setOnClick([this] { queueDecision(true); });
    absolute(*allow, horizontalPadding + buttonWidth + buttonGap, buttonY,
             buttonWidth, buttonHeight);
    card->addChild(std::move(allow));

    root->addChild(std::move(card));
    return root;
}

bool PermissionPromptSurface::show(PermissionPromptContent content,
                                   DecisionCallback callback) {
    if (surface_ || content.appName.empty() || content.title.empty() ||
        content.description.empty() || !callback) {
        return false;
    }

    content_ = std::move(content);
    callback_ = std::move(callback);
    pendingDecision_ = -1;
    surface_ = std::make_unique<lcl::ui::WindowApp>(
        lcl::render::makeDisplayListCanvas(), width_, height_,
        "Permission Prompt");
    surface_->setSurfaceId(surfaceId_);
    surface_->setSystemSurfaceKind(
        lcl::protocol::LCLSystemSurfaceKind::PermissionPrompt);
    surface_->setAppId(shellAppId_);
    surface_->setInputEnabled(true);
    surface_->setInitialBounds(0, 0, width_, height_);
    surface_->setRootWidget(makeRoot(width_, height_));
    surface_->setDecorationMode(lcl::protocol::LCLDecorationMode::None);
    surface_->setWindowCornerRadius(0.0f);
    surface_->setOnResize([this](std::uint32_t width, std::uint32_t height) {
        width_ = width;
        height_ = height;
        if (surface_) surface_->setRootWidget(makeRoot(width, height));
    });
    if (!surface_->connectCompositor()) {
        surface_.reset();
        callback_ = {};
        return false;
    }
    return true;
}

bool PermissionPromptSurface::tick() {
    if (!surface_) return false;
    const bool rendered = surface_->tick();
    if (pendingDecision_ < 0) return rendered;

    const bool allowed = pendingDecision_ != 0;
    pendingDecision_ = -1;
    surface_.reset();
    auto callback = std::move(callback_);
    if (callback) callback(allowed);
    return true;
}

bool PermissionPromptSurface::isVisible() const noexcept {
    return surface_ != nullptr;
}

void PermissionPromptSurface::dismiss() {
    pendingDecision_ = -1;
    surface_.reset();
    callback_ = {};
}

void PermissionPromptSurface::queueDecision(bool allowed) {
    if (pendingDecision_ < 0) pendingDecision_ = allowed ? 1 : 0;
}

} // namespace lcl::shell
