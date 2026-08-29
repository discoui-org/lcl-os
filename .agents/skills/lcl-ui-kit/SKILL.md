---
name: lcl-ui-kit
description: Complete technical reference, API contracts, and usage patterns for developing LCL OS user-space GUI applications using the lcl-ui framework.
---

# LCL-UI Application Development Framework Guide

`lcl-ui` is the backend-neutral C++20 user-space GUI framework for **LCL Core Linux (LCL OS)**. It creates surfaces over protocol-v27 Unix Domain `SOCK_SEQPACKET` IPC (`/Runtime/lcl-compositor.sock` by default). The compositor returns a 128-bit producer grant; the standard DisplayList Canvas sends sealed frame/resource memfds to supervised central `lcl-rasterd`, and only rasterd publishes immutable ready layers to the compositor.

---

## 1. Core Architecture Overview

```
+-----------------------------------------------------------+
|                      lcl-ui Application                   |
| (WindowApp -> Widget Tree -> LCL Layout -> DisplayList)   |
+-----------------------------+-----------------------------+
                              | sealed frame + producer grant
                              v
+-----------------------------------------------------------+
|                    Central lcl-rasterd                    |
|       (DisplayList replay -> immutable ready layer)       |
+-----------------------------+-----------------------------+
                              | private LayerReady channel
                              v
+-----------------------------------------------------------+
|                      LCL Core Compositor                  |
|       (retain -> transform/effect -> atomic present)      |
+-----------------------------------------------------------+
```

Widgets, chrome, text, clips, and effects are described in float logical
units. `RasterCanvas` records an immutable display list and rasterd applies
`RenderTarget.deviceScale` while replaying it through the platform raster
backend. Do not multiply widget geometry by display scale. The compositor never
replays application DisplayLists.

After the first complete scene, each frame is a retained DisplayList patch.
The private raster protocol carries the exact base frame serial and logical
damage union; rasterd rejects a patch whose base or geometry generation does
not match its retained per-surface scene. Daemon restart, resize, root
replacement, and frame rejection resend a complete scene. Applications never
own this recovery logic and the compositor still receives only immutable ready
layers.

`ScrollView` content is retained by rasterd as 384-logical-pixel vertical
tiles. The viewport and one tile of overscan stay resident. A pure `setScrollY`
commit changes only the ScrollContent transform; rasterd can materialize an
entering tile from its retained logical content and patch history without a new
client DisplayList or image upload. Cache-aware widgets republish their source
on daemon restart, resize, frame rejection, or other complete-scene replacement.

---

## 2. Key Framework Classes

### `lcl::ui::WindowApp` ([`window_app.hpp`](../../../lcl-ui/include/lcl-ui/core/window_app.hpp))
Manages application initialization, window surface creation, raster producer grants, IPC event processing, frame restoration after rasterd restart, and presentation pacing.

- `WindowApp(std::unique_ptr<graphics::Canvas> canvas, float width, float height, const std::string& title)`: Constructor with logical dimensions and an explicit backend-neutral Canvas.
- `void setRootWidget(std::unique_ptr<Widget> root)`: Mounts the top-level widget container.
- `bool setResizeConstraints(WindowResizeConstraints constraints)`: Declares an optional logical content-size grid before connecting. The compositor quantizes interactive toplevel resize targets so the content surface and every attached WindowGroup participant receive the same final geometry generation.
- `bool connectCompositor(const std::string& socketPath = "/Runtime/lcl-compositor.sock")`: Connects to compositor IPC and registers a v27 surface.
- `void runEventLoop()`: Runs the main non-blocking event loop at **144 Hz target frame pacing** (~6.9ms period).

### `lcl::ui::Widget` ([`widget.hpp`](../../../lcl-ui/include/lcl-ui/widgets/widget.hpp))
Base class for all UI elements.

- Type-safe layout methods (`setDirection`, `setJustifyContent`,
  `setAlignItems`, `setWidth`, `setHeight`, `setPadding`, `setMargin`,
  `setGap`, and `setPosition`) accept only `lcl::ui::layout` types. The
  underlying layout engine is a private implementation detail.
- `void addChild(std::unique_ptr<Widget> child)`: Appends a child widget.
- `void invalidatePaint()`: Invalidates painted content without scheduling layout or presentation work.
- `void invalidatePresentation()` (protected): Invalidates old/new presentation bounds without invalidating paint caches.
- `uint64_t getLayoutRevision() const`: Observes coalesced layout invalidation independently from paint and presentation revisions.
- `virtual void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect)`: Backend-neutral render callback. Widgets must not cast the Canvas to a renderer implementation.
- Intrinsically sized custom leaves derive from `MeasuredWidget`, override
  `measure(const layout::Constraints&)`, and call `invalidateMeasurement()`
  when their content size changes.
- Layout, paint, and presentation invalidation are independent. Layout setters
  do not invalidate paint; geometry changes discovered during layout sync
  invalidate presentation; transforms, opacity, visibility, clipping, and
  scroll never increment paint-cache revisions.
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
    WindowApp app(lcl::render::makeDisplayListCanvas(), 800, 600, "My LCL Application");

    // 2. Build Flexbox layout hierarchy
    auto root = std::make_unique<Container>();
    root->setWidth(800.0f);
    root->setHeight(600.0f);
    root->setDirection(layout::Direction::Column);
    root->setJustifyContent(layout::Justify::Center);
    root->setAlignItems(layout::Align::Center);
    root->setGap(16.0f);

    auto label = std::make_unique<Text>("Hello LCL OS!");
    label->setFontSize(20.0f);

    auto btn = std::make_unique<Button>("Click Me");
    btn->setWidth(140.0f);
    btn->setHeight(40.0f);
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
3. Call `invalidatePaint()` when another painted frame is required; do not depend on a fixed
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
        invalidatePaint();
    }
};
```

Client applications link both `lcl-ui` and `lcl-raster`. Compositor code must
not link `lcl-ui` or application DisplayList replay. EGL/DRM/GBM/GLES and
AHardwareBuffer selection remain below the raster-service/platform boundary.

Resize has no public presentation selector. All surfaces use strict
`AtomicRetained`: an old WindowGroup stays completely unchanged until every
size-changing parent/frame/popup layer for the newest geometry generation is
ready. Client code must not implement resize snapshots, stretch, background
reveal, or crossfade. The compositor keeps at most one atomic generation in
flight and coalesces newer pointer targets behind it; it does not repeatedly
cancel raster work that has not finished.
