---
name: lcl-ui-kit
description: Complete technical reference, API contracts, and usage patterns for developing LCL OS user-space GUI applications using the lcl-ui framework.
---

# LCL-UI Application Development Framework Guide

`lcl-ui` is the official C++20 user-space GUI framework for **LCL Core Linux (LCL OS)**. It is decoupled from the compositor, running on client processes over Unix Domain Socket IPC (`/tmp/lcl_compositor.sock`) and Zero-Copy Shared Memory (`memfd`).

---

## 1. Core Architecture Overview

```
+-----------------------------------------------------------+
|                      lcl-ui Application                   |
|  (WindowApp -> Widget Hierarchy -> Yoga Flexbox Layout)   |
+-----------------------------+-----------------------------+
                              | SHM (memfd) + Unix Domain Socket
                              v
+-----------------------------------------------------------+
|                      LCL Core Compositor                  |
|          (Direct DRM/KMS Scanout & EGL Skia Renderer)     |
+-----------------------------------------------------------+
```

---

## 2. Key Framework Classes

### `lcl::ui::WindowApp` ([`window_app.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/core/window_app.hpp))
Manages application initialization, window surface creation, SHM allocation, IPC event processing, and frame loop execution.

- `WindowApp(uint32_t width, uint32_t height, const std::string& title)`: Constructor.
- `void setRootWidget(std::unique_ptr<Widget> root)`: Mounts the top-level widget container.
- `bool connectCompositor(const std::string& socketPath = "/tmp/lcl_compositor.sock")`: Connects to compositor IPC and registers surface.
- `void runEventLoop()`: Runs the main non-blocking event loop at **144 Hz target frame pacing** (~6.9ms period).

### `lcl::ui::Widget` ([`widget.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/widgets/widget.hpp))
Base class for all UI elements.

- `YogaNode& getYogaNode()`: Accesses the C++ Yoga Flexbox layout node.
- `void addChild(std::unique_ptr<Widget> child)`: Appends a child widget.
- `void markDirty()`: Registers dirty damage bounds with `RenderPass` to trigger a frame redraw.
- `virtual void draw(SkCanvas* canvas, const Rect& damageRect)`: Virtual render callback. `canvas` is safely castable to `lcl::render::SkiaRenderer*`.
- **Event Callbacks:**
  - `virtual bool onPointerEnter(const PointerEvent& event)`
  - `virtual bool onPointerLeave(const PointerEvent& event)`
  - `virtual bool onPointerDown(const PointerEvent& event)`
  - `virtual bool onPointerUp(const PointerEvent& event)`
  - `virtual bool onPointerMove(const PointerEvent& event)`
  - `virtual bool onKeyDown(const KeyEvent& event)`
  - `virtual bool onKeyUp(const KeyEvent& event)`

### Standard Widgets
- `lcl::ui::Container`: Flexbox layout container supporting background colors, borders, padding, and gap.
- `lcl::ui::Button`: Interactive button widget with `setOnClick(std::function<void()>)`.
- `lcl::ui::Text`: TrueType vector font label supporting font sizes, colors, and string content.

---

## 3. Standard Application Skeleton

```cpp
#include <iostream>
#include <memory>
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/text.hpp"

using namespace lcl::ui;

int main() {
    // 1. Create 800x600 Window App instance
    WindowApp app(800, 600, "My LCL Application");

    // 2. Build Flexbox layout hierarchy
    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(800.0f);
    root->getYogaNode().setHeight(600.0f);
    root->getYogaNode().setDirection(YGFlexDirectionColumn);
    root->getYogaNode().setJustifyContent(YGJustifyCenter);
    root->getYogaNode().setAlignItems(YGAlignCenter);
    root->getYogaNode().setGap(YGGutterAll, 16.0f);

    auto label = std::make_unique<Text>("Hello LCL OS!");
    label->setFontSize(20.0f);

    auto btn = std::make_unique<Button>("Click Me");
    btn->getYogaNode().setWidth(140.0f);
    btn->getYogaNode().setHeight(40.0f);
    btn->setOnClick([labelPtr = label.get()]() {
        labelPtr->setText("Button Clicked!");
    });

    root->addChild(std::move(label));
    root->addChild(std::move(btn));
    app.setRootWidget(std::move(root));

    // 3. Connect to Compositor socket and run 144Hz event loop
    if (app.connectCompositor()) {
        app.runEventLoop(); // Blocks cleanly until app exits
    }

    return 0;
}
```

---

## 4. Custom Widget & Procedural Animation Protocol

For custom 2D canvas drawing or continuous procedural animations:

1. Subclass `Widget` and override `draw(SkCanvas* canvas, const Rect& damageRect)`.
2. Cast `canvas` to `lcl::render::SkiaRenderer*`.
3. Use Skia primitives:
   - `skia->drawBackgroundGradient(topColor, bottomColor)`
   - `skia->drawRect(rect, color)`
   - `skia->drawRoundedRect(rect, radius, fillColor, borderColor, borderWidth)`
   - `skia->drawDropShadow(rect, radius, blur, shadowColor)`
   - `skia->drawCircle(cx, cy, radius, color)`
   - `skia->drawString(x, y, text, fgColor)`
4. **Continuous 144Hz Animation Rule:** Call `markDirty()` **inside** `draw(...)` to request damage calculation for the next frame continuously!

```cpp
class MyAnimatedWidget : public Widget {
public:
    void draw(SkCanvas* canvas, const Rect& damageRect) override {
        (void)damageRect;
        auto* skia = reinterpret_cast<lcl::render::SkiaRenderer*>(canvas);
        if (!skia) return;

        // Custom drawing
        skia->drawCircle(100.0f, 100.0f, 40.0f, SkiaColor{255, 100, 50, 255});

        // Request next frame for continuous 144Hz animation
        markDirty();
    }
};
```
