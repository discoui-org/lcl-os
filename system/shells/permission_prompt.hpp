#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lcl::ui {
class Container;
class WindowApp;
}

namespace lcl::shell {

enum class PermissionPromptPresentation {
    Desktop,
    Mobile,
};

/** Trusted text resolved from permission metadata, never from app-supplied UI. */
struct PermissionPromptContent {
    std::string appName;
    std::string title;
    std::string description;
};

struct PermissionPromptLayout {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    bool topCornersOnly{false};
};

PermissionPromptLayout layoutPermissionPrompt(
    PermissionPromptPresentation presentation, float outputWidth,
    float outputHeight) noexcept;

/**
 * Shell-owned permission UI. Administrator privileges use this same surface;
 * only the trusted title and description differ from an ordinary permission.
 */
class PermissionPromptSurface final {
public:
    using DecisionCallback = std::function<void(bool allowed)>;

    PermissionPromptSurface(PermissionPromptPresentation presentation,
                            std::uint32_t surfaceId, std::string shellAppId,
                            std::uint32_t width, std::uint32_t height);
    ~PermissionPromptSurface();

    PermissionPromptSurface(const PermissionPromptSurface&) = delete;
    PermissionPromptSurface& operator=(const PermissionPromptSurface&) = delete;

    bool show(PermissionPromptContent content, DecisionCallback callback);
    bool tick();
    bool isVisible() const noexcept;
    void dismiss();

private:
    std::unique_ptr<lcl::ui::Container> makeRoot(std::uint32_t width,
                                                 std::uint32_t height);
    void queueDecision(bool allowed);

    PermissionPromptPresentation presentation_;
    std::uint32_t surfaceId_;
    std::string shellAppId_;
    std::uint32_t width_;
    std::uint32_t height_;
    PermissionPromptContent content_;
    DecisionCallback callback_;
    std::unique_ptr<lcl::ui::WindowApp> surface_;
    int pendingDecision_{-1};
};

} // namespace lcl::shell
