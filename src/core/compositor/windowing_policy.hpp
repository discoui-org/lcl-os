#pragma once

#include <memory>

#include "core/ipc/lcl_protocol.hpp"
#include "platform/common/gestalt.hpp"

namespace lcl::core {

/** Shell-specific policy applied to otherwise shared WindowManager mechanics. */
class WindowingPolicy {
public:
    virtual ~WindowingPolicy() = default;

    virtual lcl::platform::ShellKind shellKind() const noexcept = 0;

    virtual void configureNormalSurface(
        float outputWidth, float outputHeight,
        float& x, float& y, float& width, float& height,
        protocol::LCLDecorationMode& decorationMode,
        bool& insetBorderEnabled) const noexcept = 0;

    virtual protocol::LCLDecorationMode resolveDecorationMode(
        protocol::LCLDecorationMode requestedMode) const noexcept = 0;
    virtual bool resolveInsetBorderEnabled(bool requestedEnabled) const noexcept = 0;
    virtual float resolveWindowCornerRadius(float requestedRadius) const noexcept = 0;

    virtual bool usesDesktopWindowManagement() const noexcept = 0;
    virtual bool usesSystemGestures() const noexcept = 0;
    virtual bool usesMobileWindowDecorations() const noexcept = 0;
};

class DesktopWindowPolicy final : public WindowingPolicy {
public:
    lcl::platform::ShellKind shellKind() const noexcept override;
    void configureNormalSurface(
        float outputWidth, float outputHeight,
        float& x, float& y, float& width, float& height,
        protocol::LCLDecorationMode& decorationMode,
        bool& insetBorderEnabled) const noexcept override;
    protocol::LCLDecorationMode resolveDecorationMode(
        protocol::LCLDecorationMode requestedMode) const noexcept override;
    bool resolveInsetBorderEnabled(bool requestedEnabled) const noexcept override;
    float resolveWindowCornerRadius(float requestedRadius) const noexcept override;
    bool usesDesktopWindowManagement() const noexcept override;
    bool usesSystemGestures() const noexcept override;
    bool usesMobileWindowDecorations() const noexcept override;
};

class MobileWindowPolicy final : public WindowingPolicy {
public:
    lcl::platform::ShellKind shellKind() const noexcept override;
    void configureNormalSurface(
        float outputWidth, float outputHeight,
        float& x, float& y, float& width, float& height,
        protocol::LCLDecorationMode& decorationMode,
        bool& insetBorderEnabled) const noexcept override;
    protocol::LCLDecorationMode resolveDecorationMode(
        protocol::LCLDecorationMode requestedMode) const noexcept override;
    bool resolveInsetBorderEnabled(bool requestedEnabled) const noexcept override;
    float resolveWindowCornerRadius(float requestedRadius) const noexcept override;
    bool usesDesktopWindowManagement() const noexcept override;
    bool usesSystemGestures() const noexcept override;
    bool usesMobileWindowDecorations() const noexcept override;
};

std::unique_ptr<WindowingPolicy> makeWindowingPolicy(
    lcl::platform::ShellKind shellKind);

} // namespace lcl::core
