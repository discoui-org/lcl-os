#include "core/compositor/windowing_policy.hpp"

namespace lcl::core {

lcl::platform::ShellKind DesktopWindowPolicy::shellKind() const noexcept {
    return lcl::platform::ShellKind::Desktop;
}

void DesktopWindowPolicy::configureNormalSurface(
    float, float, float&, float&, float&, float&,
    protocol::LCLDecorationMode&, bool&) const noexcept {}

protocol::LCLDecorationMode DesktopWindowPolicy::resolveDecorationMode(
    protocol::LCLDecorationMode requestedMode) const noexcept {
    return requestedMode;
}

bool DesktopWindowPolicy::resolveInsetBorderEnabled(
    bool requestedEnabled) const noexcept {
    return requestedEnabled;
}

bool DesktopWindowPolicy::usesDesktopWindowManagement() const noexcept {
    return true;
}

bool DesktopWindowPolicy::usesSystemGestures() const noexcept {
    return false;
}

lcl::platform::ShellKind MobileWindowPolicy::shellKind() const noexcept {
    return lcl::platform::ShellKind::Mobile;
}

void MobileWindowPolicy::configureNormalSurface(
    float outputWidth, float outputHeight,
    float& x, float& y, float& width, float& height,
    protocol::LCLDecorationMode& decorationMode,
    bool& insetBorderEnabled) const noexcept {
    x = 0.0f;
    y = 0.0f;
    width = outputWidth;
    height = outputHeight;
    decorationMode = protocol::LCLDecorationMode::None;
    insetBorderEnabled = false;
}

protocol::LCLDecorationMode MobileWindowPolicy::resolveDecorationMode(
    protocol::LCLDecorationMode) const noexcept {
    return protocol::LCLDecorationMode::None;
}

bool MobileWindowPolicy::resolveInsetBorderEnabled(bool) const noexcept {
    return false;
}

bool MobileWindowPolicy::usesDesktopWindowManagement() const noexcept {
    return false;
}

bool MobileWindowPolicy::usesSystemGestures() const noexcept {
    return true;
}

std::unique_ptr<WindowingPolicy> makeWindowingPolicy(
    lcl::platform::ShellKind shellKind) {
    if (shellKind == lcl::platform::ShellKind::Mobile) {
        return std::make_unique<MobileWindowPolicy>();
    }
    return std::make_unique<DesktopWindowPolicy>();
}

} // namespace lcl::core
