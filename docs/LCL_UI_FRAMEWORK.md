# `lcl-ui` Application Development Framework & API Guide

`lcl-ui` is the official C++20 user-space GUI application framework for **LCL Core Linux (LCL OS)**. It is decoupled from display-server dependencies and provides flexbox layout, polymorphic widgets, damage tracking, and a backend-neutral Canvas contract. The standard client raster backend is selected separately through `lcl-raster`.

---

## Table of Contents
1. [Architecture Overview](#1-architecture-overview)
2. [Key Framework Classes](#2-key-framework-classes)
   - [WindowApp](#windowapp)
   - [Widget](#widget)
   - [Built-In Views and Controls](#built-in-widgets)
   - [Canvas](#lclgraphicscanvas)
3. [Building Your First Application](#3-building-your-first-application)
4. [Custom Widget Development & Procedural Animations](#4-custom-widget-development--procedural-animations)
5. [Event Handling & Input Pipeline](#5-event-handling--input-pipeline)
6. [Declarative Interaction States](#6-declarative-interaction-states)
7. [Native Themes and Explicit Styles](#7-native-themes-and-explicit-styles)

---

## 1. Architecture Overview

`lcl-ui` applications execute in user space as standalone processes. Their
normative frame boundary is a ready immutable layer. Protocol v26 creates a
surface and returns a producer grant; `WindowApp` submits sealed logical frames
to central `lcl-rasterd` over `/Runtime/lcl-raster.sock`. Only rasterd publishes
the resulting layer to `lcl-core` through its private compositor channel.

```text
+-----------------------------------------------------------+
|                      lcl-ui Application                   |
| (WindowApp -> Widget Tree -> LCL Layout -> DisplayList)   |
+-----------------------------+-----------------------------+
                              | producer-side raster
                              v
+-----------------------------------------------------------+
|        Immutable ready layer + atomic surface commit       |
+-----------------------------+-----------------------------+
                              | DMA-BUF/native buffer or SHM
                              v
+-----------------------------------------------------------+
|                      LCL Core Compositor                  |
| (Retain last layer -> transform/blend -> atomic present)  |
+-----------------------------------------------------------+
```

The compositor presentation loop never performs application layout, text
rasterization, or DisplayList replay. If an application is late, its last
committed layer remains visible and compositor-owned motion continues. A new
surface becomes visible only after its first complete layer commit.

Protocol v26 has no app-facing `CommitDisplayList`, SHM attach, DMA-BUF attach,
or native-buffer attach message. These are raster-service backend details, not
`lcl-ui` APIs.

---

## 2. Key Framework Classes

### `lcl::ui::WindowApp`
*Header:* [`lcl-ui/include/lcl-ui/core/window_app.hpp`](../lcl-ui/include/lcl-ui/core/window_app.hpp)

`WindowApp` is the top-level application container. It handles window surface
creation, raster producer-grant management, IPC message dispatching, retained
frame restoration after rasterd restart, and presentation-credit pacing.

#### Public Methods
- `WindowApp(std::unique_ptr<graphics::Canvas> canvas, float width, float height, const std::string& title = "lcl-ui Application")`: Constructs a logical-size window with an explicitly selected drawing backend.
- `void setRootWidget(std::unique_ptr<Widget> root)`: Binds the top-level Flexbox widget container.
- `Widget* getRootWidget() const`: Returns the root widget pointer.
- `void setTheme(theme::Theme theme)`: Replaces the window-owned semantic theme and propagates it through the widget tree and hosted popup surfaces.
- `const theme::Theme& getTheme() const`: Returns the active window theme.
- `void setAppId(std::string appId)`: Sets the required canonical application identity before connecting.
- `bool connectCompositor(const std::string& socketPath = "/Runtime/lcl-compositor.sock")`: Connects to `lcl-core` IPC and creates a protocol-v26 normal, popup, or attached surface.
- `registerLocalTransient(...)`: Registers an ordinary absolute-positioned Widget in the single WindowRoot tree with generic lifecycle/dismissal policy.
- `configurePopupSurface(parentSurfaceId, role, x, y)`: Configures this `WindowApp` as a compositor-level popup that reuses the normal configure and buffer path.
- `hostSurface(...)`: Owns and ticks an additional generic `WindowApp` surface from the same event loop; it contains no Popover-specific policy.
- `bool setEdgeToEdge(bool enabled)`: Extends the surface material beneath compositor-owned system insets. Desktop window controls or mobile system indicators remain foreground chrome while the client widget tree stays inside its safe content area.
- `BackdropSurface::setEffectBounds(EffectBounds::OuterSurface)`: Uses the compositor-owned outer surface rather than the local safe content rect.
- `BackdropSurface::setTint(Color color)`: Adds the tint to the same compositor filter chain as blur and color adjustment, preventing separate inset and content shades.
- `BackdropSurface::addFilter(FilterType::Glass, thickness, refractionFactor, dispersionGain)`: Declares thickness in logical units; the raster boundary applies `RenderTarget.deviceScale`, and `WindowApp` does not multiply it by `bufferScale`. Zero thickness or refraction disables Glass; zero dispersion preserves refraction without RGB color separation.
- `void runEventLoop()`: Executes the non-blocking main event loop at **144 Hz target frame pacing** (~6.9ms target period).
- `uint32_t* getPixelBuffer()`: Returns the current raster backing/staging
  buffer (`uint32_t` ARGB format) when CPU access is available.

### `Popover` v1

`Popover::show(anchor, content, options)` accepts any Widget subtree and returns
`PopoverOpenResult { handle, presentation, geometry }`. Placement is directly
below the anchor and left-aligned. Presentation is always a parent-bound hosted
`WindowApp` configured as `PopupSurface`, regardless of whether the desired
rectangle also fits inside the parent window. `TransientController` owns its
stable handle, owner teardown, outside mouse dismissal, and validated touch-tap
dismissal. `Popover::close(handle)` closes it programmatically. The constructor
accepts a backend-neutral popup `Canvas` factory, preserving the same explicit
Canvas injection contract as every other `WindowApp`. The hosted content root
is a `FocusScope`: opening clears the parent dispatcher focus and selects the
first eligible popup descendant, while close restores the weakly tracked anchor
when it is still interaction-eligible. Compositor keyboard routing tracks only
the active surface; it has no Popover or widget-focus policy.

### `FocusScope` v1

`FocusScope` is a normal `Container` that also marks a keyboard
traversal boundary without receiving focus itself. `WindowApp` routes Tab and
Shift+Tab centrally through `EventDispatcher` in depth-first insertion order.
The nearest ancestor `FocusScope` of the focused widget is the active context;
forward and backward traversal wrap inside it. Without an explicit scope, the
window root is the implicit scope. Hidden, non-focusable, or
interaction-disabled widgets are skipped. Each hosted PopupSurface has its own
`WindowApp` and therefore its own independent traversal context. Popover uses
this boundary for its transient Tab trap; ordinary form groups should remain
plain `Container` trees so window traversal can continue past them.

---

### `lcl::ui::Widget`
*Header:* [`lcl-ui/include/lcl-ui/widgets/widget.hpp`](../lcl-ui/include/lcl-ui/widgets/widget.hpp)

Base polymorphic class for all UI components.

#### Public Methods
- Type-safe layout setters such as `setDirection`, `setJustifyContent`,
  `setAlignItems`, `setWidth`, `setHeight`, `setPadding`, `setMargin`,
  `setGap`, and `setPosition` accept only types from `lcl::ui::layout`.
  The underlying layout engine and its node types are private to `lcl-ui`.
- `void addChild(std::unique_ptr<Widget> child)`: Appends a child widget into the hierarchy.
- `void removeChild(Widget* child)`: Removes a child widget.
- `const std::vector<std::unique_ptr<Widget>>& getChildren() const`: Returns list of children.
- `void invalidatePaint()`: Invalidates painted content without scheduling layout or presentation work.
- `void invalidatePresentation()` (protected): Damages old/new presentation bounds without invalidating paint caches.
- `uint64_t getLayoutRevision() const`: Observes layout invalidation independently from paint and presentation revisions.
- `void setVisible(bool visible)`: Controls widget visibility.
- `virtual void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect)`: Virtual render method called during damage passes through the backend-neutral Canvas contract.
- `void setInteractionStyle(InteractionState state, InteractionStyle style)`: Defines presentation-only pseudo-state values for custom controls.
- `void useStyle(theme::WidgetStyle style)`: Applies one explicit native style to the widget; no selector matching or cascade is involved.
- `void clearStyle()`: Restores the control's semantic theme style.
- `const theme::Theme& getTheme() const`: Returns the inherited window theme.
- `virtual void setOnClick(std::function<void()> callback)`: Makes any widget clickable without a pointer-event subclass.
- `void setInteractionEnabled(bool enabled)`: Enables or disables declarative pointer behavior.

Invalidation channels are independent. Layout setters and intrinsic
measurement changes schedule layout; pixel content and style changes call
`invalidatePaint()`; transforms, opacity, visibility, clipping, and scroll
offsets invalidate presentation. A content mutation such as changing measured
text explicitly schedules both layout and paint. Layout synchronization only
invalidates presentation when the computed geometry actually changes.

The first raster submission is a complete surface DisplayList. Later
submissions contain only clipped commands for the accumulated damage regions
and identify the exact retained raster frame they patch. `lcl-rasterd` keeps
the authoritative per-surface scene, rejects stale-base patches, and publishes
a new immutable layer with pixel-space damage. Resize, daemon restart, root
replacement, or a rejected frame automatically forces a complete replacement.
The compositor never interprets or replays application DisplayLists.

### `lcl::ui::MeasuredWidget`

Custom leaf widgets with intrinsic content size derive from `MeasuredWidget`
and implement `measure(const layout::Constraints&)`. The width and height
constraints carry an LCL-owned `MeasureMode` (`Undefined`, `Exactly`, or
`AtMost`). Call `invalidateMeasurement()` whenever content that affects the
intrinsic size changes.

```cpp
class Swatch final : public MeasuredWidget {
protected:
    layout::Size measure(const layout::Constraints&) override {
        return {24.0f, 24.0f};
    }
};
```

---

### Built-In Widgets

#### `lcl::ui::Container` ([`container.hpp`](../lcl-ui/include/lcl-ui/widgets/container.hpp))
A layout container supporting background colors, border colors, padding, and corner rounding.

#### `lcl::ui::Button` ([`button.hpp`](../lcl-ui/include/lcl-ui/widgets/button.hpp))
An interactive button component supporting hover/pressed states and click callbacks:
```cpp
auto btn = std::make_unique<Button>("Click Me");
btn->setOnClick([]() {
    std::cout << "Button clicked!\n";
});
```

#### `lcl::ui::Text` ([`text.hpp`](../lcl-ui/include/lcl-ui/widgets/text.hpp))
A vector typography label component supporting custom font sizes and text content:
```cpp
auto label = std::make_unique<Text>("Hello LCL OS");
label->setFontSize(18.0f);
label->setTextColor(0xFFFFFFFF);
```

#### Selection and value controls

- `Toggle` supports `Automatic`, `Switch`, `Checkbox`, and `Button`
  presentation styles while keeping one Boolean value and change callback.
- `Picker` owns one mutually-exclusive selection and supports `Menu` and
  `RadioGroup` styles. The menu style reuses the parent-bound popup surface
  path used by `Menu` and `Popover`.
- `Slider` selects a value from a bounded range, supports optional step
  quantization, live change callbacks, editing-state callbacks, pointer
  capture, and keyboard adjustment.
- `ProgressView` supports determinate and indeterminate progress with linear
  and circular styles.
- `TabView` switches between `Tab` content trees while preserving the state of
  each tree and presents a persistent tab bar for top-level navigation.
- `Divider` draws a semantic separator and adapts to its laid-out aspect.

Checkboxes are a `Toggle` style rather than a separate value model. Radio
groups and menu-based dropdown presentation are `Picker` styles rather than
separate controls. All of these controls draw logical geometry through
`graphics::Canvas`; no control type is exposed to the raster renderer.

---

### `lcl::graphics::Canvas`
*Header:* [`lcl-graphics/include/lcl-graphics/canvas.hpp`](../lcl-graphics/include/lcl-graphics/canvas.hpp)

The backend-neutral 2D drawing contract available inside `Widget::draw(...)`:
- `drawRect(rect, color)`
- `drawRoundedRect(rect, radius, fillColor, borderColor, borderWidth, roundness)`
- `drawTopRoundedRect(rect, radius, color, roundness)`
- `drawPath(path, paint)` and `drawEllipse(rect, paint)`
- `drawText(x, y, text, color, fontSize)`
- `drawBuffer(...)`

Geometry, text sizes, radii, strokes, clips, and effects use float logical
units. `RasterCanvas` records them into one immutable `DisplayList`; `endFrame()`
closes the logical frame and hands it to the producer-side raster stage.
`RasterRenderer` applies `RenderTarget.deviceScale` while producing the
immutable layer that will be atomically committed. Cached application layers
are raster resources, not command streams replayed by the compositor on every
presentation frame. Text measurement and GPU/CPU drawing share the same Skia
font engine.

Applications select the standard adapter explicitly with
`lcl::render::makeDisplayListCanvas()` and link `lcl-raster`. Widgets themselves
depend only on `Canvas`; `lcl-ui` does not link EGL, DRM, GBM, or GLES.
The adapter serializes the sealed logical frame only to central `lcl-rasterd`.
The compositor never decodes or replays application DisplayLists.

---

## 3. Building Your First Application

Create `main.cpp` inside `apps/my_app/`:

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
    // 1. Initialize WindowApp (600x400)
    WindowApp app(lcl::render::makeDisplayListCanvas(), 600, 400, "My Application");

    // 2. Build Centered Flexbox Layout Tree
    auto rootContainer = std::make_unique<Container>();
    rootContainer->setWidth(600.0f);
    rootContainer->setHeight(400.0f);
    rootContainer->setDirection(layout::Direction::Column);
    rootContainer->setJustifyContent(layout::Justify::Center);
    rootContainer->setAlignItems(layout::Align::Center);
    rootContainer->setGap(16.0f);

    auto statusText = std::make_unique<Text>("Click counter: 0");
    statusText->setFontSize(18.0f);
    Text* textPtr = statusText.get();

    auto actionButton = std::make_unique<Button>("Click Me");
    actionButton->setWidth(140.0f);
    actionButton->setHeight(40.0f);

    static int counter = 0;
    actionButton->setOnClick([textPtr]() {
        counter++;
        textPtr->setText("Click counter: " + std::to_string(counter));
    });

    rootContainer->addChild(std::move(statusText));
    rootContainer->addChild(std::move(actionButton));
    app.setRootWidget(std::move(rootContainer));

    // 3. Connect to Compositor IPC and run 144Hz event loop
    if (app.connectCompositor()) {
        app.runEventLoop(); // Blocks cleanly until application quits
    }

    return 0;
}
```

---

## 4. Custom Widget Development & Procedural Animations

For continuous procedural 2D canvas drawing or custom animations:

1. Inherit from `Widget` and override `draw(graphics::Canvas& canvas, const graphics::RectF& damageRect)`.
2. Use Canvas primitives such as `drawRect`, `drawRoundedRect`, `drawText`, and `drawBuffer`.
3. Call `invalidatePaint()` **inside** `draw(...)` to continuously request painted damage for the next 144Hz frame.
4. Launch the application using `app.runEventLoop()`.

```cpp
class AnimatedWidget : public Widget {
public:
    AnimatedWidget() {
        m_startTime = std::chrono::steady_clock::now();
    }

    void draw(lcl::graphics::Canvas& canvas,
              const lcl::graphics::RectF& damageRect) override {
        (void)damageRect;

        // Draw an animated pulse through the portable Canvas contract.
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - m_startTime).count();
        float side = 60.0f + std::sin(t * 4.0f) * 20.0f;
        canvas.drawRect({150.0f - side * 0.5f, 150.0f - side * 0.5f, side, side},
                        {56, 189, 248, 255});

        // Request next frame for continuous 144Hz animation
        invalidatePaint();
    }

private:
    std::chrono::steady_clock::time_point m_startTime;
};
```

---

## 5. Event Handling & Input Pipeline

`lcl-ui` routes input events top-down through the widget tree via `EventDispatcher`:

- **Pointer Events:** `onPointerEnter`, `onPointerLeave`, `onPointerDown`, `onPointerUp`, `onPointerMove`.
- **Keyboard Events:** `onKeyDown`, `onKeyUp`, `onTextInput`.
- **Raw Event Interceptors:** `WindowApp::setOnRawKeyEvent()`, `WindowApp::setOnRawPointerEvent()`.

---

## 6. Declarative Interaction States

Any widget can opt into native `normal`, `hover`, `pressed`, `focused`, and
`disabled` presentation states. State changes retarget the shared motion engine,
so rapid pointer movement preserves spring continuity. Model layout values are
not mutated by these effects.

```cpp
auto control = std::make_unique<Container>();
control->setWidth(160.0f);
control->setHeight(44.0f);
control->setInteractionStyle(InteractionState::Normal,
    InteractionStyle{.scale = 1.0f, .opacity = 1.0f, .motion = std::nullopt});
control->setInteractionStyle(InteractionState::Hover,
    InteractionStyle{.scale = 1.015f, .opacity = 1.0f,
                     .motion = Motion::spring(0.18f, 0.0f)});
control->setInteractionStyle(InteractionState::Pressed,
    InteractionStyle{.scale = 0.965f, .opacity = 0.92f,
                     .motion = Motion::spring(0.10f, 0.0f)});
control->setOnClick([] { std::cout << "Activated\n"; });
```

The QuickJS contract accepts the same states. A state may use a physical spring
or a tween with any named easing, `cubic-bezier(...)`, or `steps(...)`:

```js
const control = new LCL.Container();
control.setWidth(160);
control.setHeight(44);
control.setInteractionStyle("hover", {
    scale: 1.015,
    motion: {type: "spring", duration: 180, bounce: 0}
});
control.setInteractionStyle("pressed", {
    scale: 0.965,
    opacity: 0.92,
    motion: {type: "tween", duration: 100, easing: "cubic-bezier(0.2,0,0,1)"}
});
control.setOnClick(() => console.log("Activated"));
```

---

## 7. Native Themes and Explicit Styles

`WindowApp` owns one `theme::ThemeContext`. Its semantic colors and control
styles are inherited by the root widget, descendants, and hosted popup
surfaces. `Text`, `Button`, `TextField`, `Toggle`, menu items, and popover
panels resolve their defaults from that context. TextField placeholder/caret
colors and Toggle off/on track colors use the same semantic values rather than
embedding palette constants in widget drawing code.

Use `Widget::useStyle()` for a local code-defined override. It replaces the
control's semantic default as one value object; there are no selectors,
specificity rules, stylesheets, or cascade.

```cpp
auto quietButton = std::make_unique<Button>("Details");
auto quietStyle = app.getTheme().button;
quietStyle.normal.background = app.getTheme().colors.elevatedSurface;
quietStyle.normal.border = app.getTheme().colors.separator;
quietStyle.hover.background = graphics::Color{58, 58, 60, 255};
quietStyle.pressed.background = graphics::Color{36, 36, 38, 255};
quietButton->useStyle(std::move(quietStyle));
```

Call `clearStyle()` to return to the active theme. Replacing the window theme
with `WindowApp::setTheme()` refreshes the complete widget tree without
introducing rendering-backend or device-scale knowledge into control code.
`Text::setTextColor()` remains an explicit application override;
`Text::resetTextColor()` returns that label to its inherited theme color.
