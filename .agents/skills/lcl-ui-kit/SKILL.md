---
name: lcl-ui-kit
description: Complete technical reference, API contracts, and usage patterns for developing LCL OS user-space GUI applications using the lcl-ui framework.
---

# LCL-UI Application Development Framework Guide

`lcl-ui` is the backend-neutral C++20 user-space GUI framework for **LCL Core Linux (LCL OS)**. It runs in client processes over protocol-v13 Unix Domain `SOCK_SEQPACKET` IPC (`/Runtime/lcl-compositor.sock` by default), using DMA-BUF transport with a lazy shared-memory (`memfd`) fallback. Applications explicitly inject a `lcl::graphics::Canvas`; the standard client adapter is provided by `lcl-raster`.

---

## 1. Core Architecture Overview

```
+-----------------------------------------------------------+
|                      lcl-ui Application                   |
| (WindowApp -> Widget Tree -> Yoga -> logical DisplayList) |
+-----------------------------+-----------------------------+
                              | DMA-BUF or SHM + Unix Domain Socket
                              v
+-----------------------------------------------------------+
|                      LCL Core Compositor                  |
|        (Direct DRM/KMS scanout + LCL raster replay)       |
+-----------------------------------------------------------+
```

Widgets, chrome, text, clips, and effects are described in float logical
units. `RasterCanvas` records an immutable display list and
`RasterRenderer` applies `RenderTarget.deviceScale` while replaying it through
the GLES or software path. Do not multiply widget geometry by display scale.

---

## 2. Key Framework Classes

### `lcl::ui::WindowApp` ([`window_app.hpp`](../../../lcl-ui/include/lcl-ui/core/window_app.hpp))
Manages application initialization, window surface creation, DMA-BUF allocation with lazy SHM fallback, IPC event processing, and frame loop execution.

- `WindowApp(std::unique_ptr<graphics::Canvas> canvas, float width, float height, const std::string& title)`: Constructor with logical dimensions and an explicit backend-neutral Canvas.
- `void setRootWidget(std::unique_ptr<Widget> root)`: Mounts the top-level widget container.
- `bool connectCompositor(const std::string& socketPath = "/Runtime/lcl-compositor.sock")`: Connects to compositor IPC and registers a v13 surface.
- `void runEventLoop()`: Runs the main non-blocking event loop at **144 Hz target frame pacing** (~6.9ms period).

### `lcl::ui::Widget` ([`widget.hpp`](../../../lcl-ui/include/lcl-ui/widgets/widget.hpp))
Base class for all UI elements.

- `YogaNode& getYogaNode()`: Accesses the C++ Yoga Flexbox layout node.
- `void addChild(std::unique_ptr<Widget> child)`: Appends a child widget.
- `void markDirty()`: Registers dirty damage bounds with `RenderPass` to trigger a frame redraw.
- `virtual void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect)`: Backend-neutral render callback. Widgets must not cast the Canvas to a renderer implementation.
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
#include "render/raster_canvas.hpp"

using namespace lcl::ui;

int main() {
    // 1. Create 800x600 Window App instance
    WindowApp app(lcl::render::makeRasterCanvas(), 800, 600, "My LCL Application");

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

For custom 2D Canvas drawing or continuous procedural animations:

1. Subclass `Widget` and override `draw(graphics::Canvas& canvas, const graphics::RectF& damageRect)`.
2. Use only Canvas primitives such as `drawRect`, `drawRoundedRect`,
   `drawTopRoundedRect`, `drawText`, and `drawBuffer`.
3. Call `markDirty()` when another frame is required; do not depend on a fixed
   refresh rate or cast Canvas to `RasterRenderer`.

```cpp
class MyAnimatedWidget : public Widget {
public:
    void draw(lcl::graphics::Canvas& canvas,
              const lcl::graphics::RectF& damageRect) override {
        (void)damageRect;
        canvas.drawRoundedRect(
            {60.0f, 60.0f, 80.0f, 80.0f}, 40.0f,
            {255, 100, 50, 255}, {}, 0.0f, 2.0f);
        markDirty();
    }
};
```

Client applications link both `lcl-ui` and `lcl-raster`. Compositor code links
the rendering, graphics, and chrome layers directly; it must not link `lcl-ui`.
EGL/DRM/GBM/GLES remain below the `lcl-ui` boundary.
