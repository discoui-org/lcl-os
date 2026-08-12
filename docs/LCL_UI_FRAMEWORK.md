# `lcl-ui` Application Development Framework & API Guide

`lcl-ui` is the official C++20 user-space GUI application framework for **LCL Core Linux (LCL OS)**. It is decoupled from display-server dependencies and provides flexbox layout, polymorphic widgets, damage tracking, and a backend-neutral Canvas contract. The standard client raster backend is selected separately through `lcl-canvas-skia`.

---

## Table of Contents
1. [Architecture Overview](#1-architecture-overview)
2. [Key Framework Classes](#2-key-framework-classes)
   - [WindowApp](#windowapp)
   - [Widget](#widget)
   - [Built-In Widgets (Container, Button, Text)](#built-in-widgets)
   - [Canvas](#lcluicanvas)
3. [Building Your First Application](#3-building-your-first-application)
4. [Custom Widget Development & Procedural Animations](#4-custom-widget-development--procedural-animations)
5. [Event Handling & Input Pipeline](#5-event-handling--input-pipeline)

---

## 1. Architecture Overview

`lcl-ui` applications execute in user space as standalone processes and communicate with the `lcl-core` Compositor via protocol-v3 Unix Domain `SOCK_SEQPACKET` IPC (`/run/user/1000/lcl-compositor.sock`) and shared memory (`memfd`).

```text
+-----------------------------------------------------------+
|                      lcl-ui Application                   |
|  (WindowApp -> Widget Tree -> Yoga -> injected Canvas)    |
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

### `lcl::ui::WindowApp`
*Header:* [`lcl-ui/include/lcl-ui/core/window_app.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/core/window_app.hpp)

`WindowApp` is the top-level application container. It handles window surface creation, SHM pixel allocation, IPC message dispatching, and 144Hz frame pacing.

#### Public Methods
- `WindowApp(std::unique_ptr<Canvas> canvas, uint32_t width, uint32_t height, const std::string& title = "lcl-ui Application")`: Constructs a window with an explicitly selected drawing backend.
- `void setRootWidget(std::unique_ptr<Widget> root)`: Binds the top-level Flexbox widget container.
- `Widget* getRootWidget() const`: Returns the root widget pointer.
- `bool connectCompositor(const std::string& socketPath = "/run/user/1000/lcl-compositor.sock")`: Connects to `lcl-core` IPC and creates a v3 window surface.
- `void runEventLoop()`: Executes the non-blocking main event loop at **144 Hz target frame pacing** (~6.9ms target period).
- `uint32_t* getPixelBuffer()`: Returns raw pointer to the SHM pixel buffer (`uint32_t` ARGB format).

---

### `lcl::ui::Widget`
*Header:* [`lcl-ui/include/lcl-ui/widgets/widget.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/widgets/widget.hpp)

Base polymorphic class for all UI components.

#### Public Methods
- `YogaNode& getYogaNode()`: Accesses the Yoga Flexbox node to configure layout attributes (`setWidth`, `setHeight`, `setPadding`, `setGap`, `setDirection`, `setJustifyContent`, `setAlignItems`).
- `void addChild(std::unique_ptr<Widget> child)`: Appends a child widget into the hierarchy.
- `void removeChild(Widget* child)`: Removes a child widget.
- `const std::vector<std::unique_ptr<Widget>>& getChildren() const`: Returns list of children.
- `void markDirty()`: Registers dirty damage bounds with `RenderPass` to schedule a frame redraw.
- `void setVisible(bool visible)`: Controls widget visibility.
- `virtual void draw(Canvas& canvas, const Rect& damageRect)`: Virtual render method called during damage passes through the backend-neutral Canvas contract.

---

### Built-In Widgets

#### `lcl::ui::Container` ([`container.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/widgets/container.hpp))
A layout container supporting background colors, border colors, padding, and corner rounding.

#### `lcl::ui::Button` ([`button.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/widgets/button.hpp))
An interactive button component supporting hover/pressed states and click callbacks:
```cpp
auto btn = std::make_unique<Button>("Click Me");
btn->setOnClick([]() {
    std::cout << "Button clicked!\n";
});
```

#### `lcl::ui::Text` ([`text.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/widgets/text.hpp))
A vector typography label component supporting custom font sizes and text content:
```cpp
auto label = std::make_unique<Text>("Hello LCL OS");
label->setFontSize(18.0f);
label->setTextColor(0xFF38BDF8); // Sky Cyan
```

---

### `lcl::ui::Canvas`
*Header:* [`lcl-ui/include/lcl-ui/core/canvas.hpp`](file:///home/superb/Projects/lcl-os/lcl-ui/include/lcl-ui/core/canvas.hpp)

The backend-neutral 2D drawing contract available inside `Widget::draw(...)`:
- `drawRect(rect, color)`
- `drawRoundedRect(rect, radius, fillColor, borderColor, borderWidth, roundness)`
- `drawTopRoundedRect(rect, radius, color, roundness)`
- `drawText(x, y, text, color, fontSize)`
- `drawBuffer(...)`

`SkiaCanvas` is the standard client SHM raster adapter. Applications select it
explicitly with `lcl::render::makeSkiaCanvas()` and link `lcl-canvas-skia`.
Widgets themselves depend only on `Canvas`; `lcl-ui` does not link EGL, DRM,
GBM, or GLES.

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
#include "render/skia_canvas.hpp"

using namespace lcl::ui;

int main() {
    // 1. Initialize WindowApp (600x400)
    WindowApp app(lcl::render::makeSkiaCanvas(), 600, 400, "My Application");

    // 2. Build Centered Flexbox Layout Tree
    auto rootContainer = std::make_unique<Container>();
    rootContainer->getYogaNode().setWidth(600.0f);
    rootContainer->getYogaNode().setHeight(400.0f);
    rootContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
    rootContainer->getYogaNode().setJustifyContent(YGJustifyCenter);
    rootContainer->getYogaNode().setAlignItems(YGAlignCenter);
    rootContainer->getYogaNode().setGap(YGGutterAll, 16.0f);

    auto statusText = std::make_unique<Text>("Click counter: 0");
    statusText->setFontSize(18.0f);
    Text* textPtr = statusText.get();

    auto actionButton = std::make_unique<Button>("Click Me");
    actionButton->getYogaNode().setWidth(140.0f);
    actionButton->getYogaNode().setHeight(40.0f);

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

1. Inherit from `Widget` and override `draw(Canvas& canvas, const Rect& damageRect)`.
2. Use Canvas primitives such as `drawRect`, `drawRoundedRect`, `drawText`, and `drawBuffer`.
3. Call `markDirty()` **inside** `draw(...)` to continuously request damage recalculation for the next 144Hz frame.
4. Launch the application using `app.runEventLoop()`.

```cpp
class AnimatedWidget : public Widget {
public:
    AnimatedWidget() {
        m_startTime = std::chrono::steady_clock::now();
    }

    void draw(Canvas& canvas, const Rect& damageRect) override {
        (void)damageRect;

        // Draw an animated pulse through the portable Canvas contract.
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - m_startTime).count();
        float side = 60.0f + std::sin(t * 4.0f) * 20.0f;
        canvas.drawRect({150.0f - side * 0.5f, 150.0f - side * 0.5f, side, side},
                        {56, 189, 248, 255});

        // Request next frame for continuous 144Hz animation
        markDirty();
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
