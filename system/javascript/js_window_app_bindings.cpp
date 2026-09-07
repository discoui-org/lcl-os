#include "system/javascript/js_binding_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/core/animation.hpp"
#include "system/render/raster_canvas.hpp"

namespace lcl::binding {
namespace {

lcl::ui::WindowApp* window(JSContext* ctx, JSValueConst value) {
    return detail::requireWindowApp(ctx, value);
}

std::optional<std::string> stringValue(JSContext* ctx, JSValueConst value) {
    const char* text = JS_ToCString(ctx, value);
    if (!text) return std::nullopt;
    std::string result(text);
    JS_FreeCString(ctx, text);
    return result;
}

bool numberValue(JSContext* ctx, JSValueConst value, double& result) {
    return JS_ToFloat64(ctx, &result, value) == 0 && std::isfinite(result);
}

JSValue constructor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    double width = 800.0, height = 600.0;
    if ((argc > 0 && !numberValue(ctx, argv[0], width)) ||
        (argc > 1 && !numberValue(ctx, argv[1], height)) ||
        width <= 0.0 || height <= 0.0)
        return JS_ThrowRangeError(ctx, "WindowApp dimensions must be positive finite numbers");
    const auto title = argc > 2 ? stringValue(ctx, argv[2])
                                : std::optional<std::string>{"LCL JS App"};
    if (!title) return JS_EXCEPTION;
    return detail::wrapWindowApp(ctx, target, new lcl::ui::WindowApp(
        lcl::render::makeDisplayListCanvas(), static_cast<float>(width),
        static_cast<float>(height), *title));
}

JSValue getWidth(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewFloat64(ctx, target->getWidth()) : JS_EXCEPTION;
}
JSValue getHeight(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewFloat64(ctx, target->getHeight()) : JS_EXCEPTION;
}
JSValue getBufferScale(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewFloat64(ctx, target->getBufferScale()) : JS_EXCEPTION;
}
JSValue getTitle(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewString(ctx, target->getTitle().c_str()) : JS_EXCEPTION;
}
JSValue getAppId(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewString(ctx, target->getAppId().c_str()) : JS_EXCEPTION;
}
JSValue isConnected(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewBool(ctx, target->isIpcConnected()) : JS_EXCEPTION;
}

JSValue connect(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc == 0) return JS_NewBool(ctx, target->connectCompositor());
    const auto path = stringValue(ctx, argv[0]); if (!path) return JS_EXCEPTION;
    return JS_NewBool(ctx, target->connectCompositor(*path));
}

JSValue setRootWidget(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "root widget is required");
    auto root = detail::takeWidget(ctx, argv[0]); if (!root) return JS_EXCEPTION;
    target->setRootWidget(std::move(root)); return JS_UNDEFINED;
}
JSValue runEventLoop(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    target->runEventLoop(); return JS_UNDEFINED;
}
JSValue tick(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewBool(ctx, target->tick()) : JS_EXCEPTION;
}
JSValue renderFrame(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewBool(ctx, target->renderFrame()) : JS_EXCEPTION;
}
JSValue setSurfaceId(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    int32_t value = 0;
    if (argc < 1 || JS_ToInt32(ctx, &value, argv[0]) < 0 || value <= 0)
        return JS_ThrowRangeError(ctx, "surface id must be positive");
    target->setSurfaceId(static_cast<uint32_t>(value));
    return JS_NewBool(ctx, target->getSurfaceId() == static_cast<uint32_t>(value));
}
JSValue setAppId(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value || value->empty() || value->size() >= 64)
        return JS_ThrowRangeError(ctx, "app id must contain 1..63 bytes");
    target->setAppId(*value); return JS_UNDEFINED;
}

JSValue sendPointer(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int kind) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    double x = 0.0, y = 0.0; int32_t button = 0;
    if (argc < 2 || !numberValue(ctx, argv[0], x) || !numberValue(ctx, argv[1], y))
        return JS_ThrowTypeError(ctx, "pointer coordinates are required");
    if (argc > 2) JS_ToInt32(ctx, &button, argv[2]);
    bool handled = false;
    if (kind == 0) handled = target->sendPointerMove(static_cast<float>(x), static_cast<float>(y));
    else if (kind == 1) handled = target->sendPointerDown(static_cast<float>(x), static_cast<float>(y), button);
    else handled = target->sendPointerUp(static_cast<float>(x), static_cast<float>(y), button);
    return JS_NewBool(ctx, handled);
}
JSValue pointerMove(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return sendPointer(c, s, a, v, 0); }
JSValue pointerDown(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return sendPointer(c, s, a, v, 1); }
JSValue pointerUp(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return sendPointer(c, s, a, v, 2); }

JSValue windowAction(JSContext* ctx, JSValueConst self, int action, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    bool result = false;
    switch (action) {
        case 0: {
            double x = 0.0, y = 0.0;
            if (argc < 2 || !numberValue(ctx, argv[0], x) || !numberValue(ctx, argv[1], y))
                return JS_ThrowTypeError(ctx, "window drag requires x and y");
            result = target->requestWindowDrag(static_cast<float>(x), static_cast<float>(y)); break;
        }
        case 1: result = target->requestWindowMinimize(); break;
        case 2: result = target->requestWindowMaximize(); break;
        case 3: result = target->requestWindowRestore(); break;
        case 4: result = target->requestWindowToggleMaximize(); break;
        case 5: result = target->requestWindowClose(); break;
    }
    return JS_NewBool(ctx, result);
}
JSValue drag(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return windowAction(c, s, 0, a, v); }
JSValue minimize(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return windowAction(c, s, 1, a, v); }
JSValue maximize(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return windowAction(c, s, 2, a, v); }
JSValue restore(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return windowAction(c, s, 3, a, v); }
JSValue toggleMaximize(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return windowAction(c, s, 4, a, v); }
JSValue close(JSContext* c, JSValueConst s, int a, JSValueConst* v) { return windowAction(c, s, 5, a, v); }

JSValue decoration(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "decoration mode is required");
    auto mode = lcl::protocol::LCLDecorationMode::SSD;
    if (*value == "csd") mode = lcl::protocol::LCLDecorationMode::CSD;
    else if (*value == "none" || *value == "frameless") mode = lcl::protocol::LCLDecorationMode::None;
    else if (*value != "ssd") return JS_ThrowRangeError(ctx, "unknown decoration mode");
    return JS_NewBool(ctx, target->setDecorationMode(mode));
}
JSValue edgeToEdge(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "enabled state is required");
    return JS_NewBool(ctx, target->setEdgeToEdge(JS_ToBool(ctx, argv[0]) != 0));
}
JSValue cornerStyle(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    double radius = 0.0, roundness = 2.0;
    if (argc < 1 || !numberValue(ctx, argv[0], radius) ||
        (argc > 1 && !numberValue(ctx, argv[1], roundness)) || radius < 0.0)
        return JS_ThrowRangeError(ctx, "corner style requires non-negative radius and finite roundness");
    return JS_NewBool(ctx, target->setWindowCornerStyle(static_cast<float>(radius), static_cast<float>(roundness)));
}

JSValue animate(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "animate requires options and callback");
    auto numberProperty = [&](const char* name, double fallback) {
        JSValue value = JS_GetPropertyStr(ctx, argv[0], name);
        double result = fallback;
        if (!JS_IsUndefined(value)) numberValue(ctx, value, result);
        JS_FreeValue(ctx, value);
        return result;
    };
    JSValue typeValue = JS_GetPropertyStr(ctx, argv[0], "type");
    const auto type = JS_IsUndefined(typeValue)
        ? std::optional<std::string>{"spring"} : stringValue(ctx, typeValue);
    JS_FreeValue(ctx, typeValue);
    if (!type) return JS_EXCEPTION;

    const float duration = static_cast<float>(std::max(
        0.0, numberProperty("duration", 180.0)) / 1000.0);
    lcl::motion::Motion motion;
    if (*type == "tween") {
        JSValue easingValue = JS_GetPropertyStr(ctx, argv[0], "easing");
        const auto easing = JS_IsUndefined(easingValue)
            ? std::optional<std::string>{"easeOutCubic"}
            : stringValue(ctx, easingValue);
        JS_FreeValue(ctx, easingValue);
        if (!easing) return JS_EXCEPTION;
        motion = lcl::motion::Motion::tween(
            duration, parseAnimationEasing(*easing),
            static_cast<float>(std::max(0.0, numberProperty("delay", 0.0)) / 1000.0));
    } else {
        const double stiffness = numberProperty("stiffness", -1.0);
        if (stiffness > 0.0) {
            motion = lcl::motion::Motion::spring(
                static_cast<float>(numberProperty("mass", 1.0)),
                static_cast<float>(stiffness),
                static_cast<float>(numberProperty("damping", 36.0)),
                static_cast<float>(numberProperty("initialVelocity", 0.0)));
        } else {
            motion = lcl::motion::Motion::spring(
                duration, static_cast<float>(numberProperty("bounce", 0.0)));
        }
    }

    JSValue layoutValue = JS_GetPropertyStr(ctx, argv[0], "layout");
    const auto layout = JS_IsUndefined(layoutValue)
        ? std::optional<std::string>{"reflow"} : stringValue(ctx, layoutValue);
    JS_FreeValue(ctx, layoutValue);
    if (!layout) return JS_EXCEPTION;
    lcl::ui::AnimationTransactionOptions options;
    options.layout = *layout == "morph" ? lcl::ui::LayoutMode::Morph
                                          : lcl::ui::LayoutMode::Reflow;
    JSValue callbackResult = JS_UNDEFINED;
    target->animate(motion, options, [&] {
        callbackResult = JS_Call(ctx, argv[1], JS_UNDEFINED, 0, nullptr);
    });
    if (JS_IsException(callbackResult)) return callbackResult;
    JS_FreeValue(ctx, callbackResult); return JS_UNDEFINED;
}

JSValue resize(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    double width = 0.0, height = 0.0;
    if (argc < 2 || !numberValue(ctx, argv[0], width) || !numberValue(ctx, argv[1], height) ||
        width <= 0.0 || height <= 0.0)
        return JS_ThrowRangeError(ctx, "resize requires positive width and height");
    target->resize(static_cast<float>(width), static_cast<float>(height)); return JS_UNDEFINED;
}

JSValue initialBounds(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    std::array<double, 4> values{};
    if (argc < 4) return JS_ThrowTypeError(ctx, "setInitialBounds requires x, y, width, height");
    for (size_t index = 0; index < values.size(); ++index)
        if (!numberValue(ctx, argv[index], values[index])) return JS_ThrowTypeError(ctx, "bounds must be finite");
    target->setInitialBounds(static_cast<float>(values[0]), static_cast<float>(values[1]),
                             static_cast<float>(values[2]), static_cast<float>(values[3]));
    return JS_UNDEFINED;
}

JSValue resizeConstraints(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "constraints object is required");
    auto read = [&](const char* name, double fallback) {
        JSValue value = JS_GetPropertyStr(ctx, argv[0], name);
        double result = fallback;
        if (!JS_IsUndefined(value)) numberValue(ctx, value, result);
        JS_FreeValue(ctx, value);
        return result;
    };
    return JS_NewBool(ctx, target->setResizeConstraints({
        static_cast<float>(read("baseWidth", 0.0)), static_cast<float>(read("baseHeight", 0.0)),
        static_cast<float>(read("widthIncrement", 0.0)), static_cast<float>(read("heightIncrement", 0.0)),
    }));
}

JSValue setInputEnabled(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "enabled state is required");
    target->setInputEnabled(JS_ToBool(ctx, argv[0]) != 0); return JS_UNDEFINED;
}
JSValue getInputEnabled(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); return target ? JS_NewBool(ctx, target->isInputEnabled()) : JS_EXCEPTION;
}

JSValue setOnResize(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "resize callback is required");
    auto store = detail::setWindowCallback(ctx, self, "window.resize", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnResize(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? lcl::ui::ResizeCallback{}
        : lcl::ui::ResizeCallback([store](float width, float height) {
            detail::invokeTwoNumberCallback(store, "window.resize", width, height);
        }));
    return JS_UNDEFINED;
}

JSValue setOnFrame(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "frame callback is required");
    auto store = detail::setWindowCallback(ctx, self, "window.frame", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnFrame(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? lcl::ui::FrameCallback{}
        : lcl::ui::FrameCallback([store] { detail::invokeCallback(store, "window.frame"); }));
    return JS_UNDEFINED;
}

const char* pointerTypeName(lcl::ui::PointerEventType type) {
    switch (type) {
        case lcl::ui::PointerEventType::Move: return "move";
        case lcl::ui::PointerEventType::Down: return "down";
        case lcl::ui::PointerEventType::Up: return "up";
        case lcl::ui::PointerEventType::Enter: return "enter";
        case lcl::ui::PointerEventType::Leave: return "leave";
        case lcl::ui::PointerEventType::Scroll: return "scroll";
        case lcl::ui::PointerEventType::Cancel: return "cancel";
    }
    return "move";
}

JSValue setOnRawPointerEvent(JSContext* ctx, JSValueConst self, int argc,
                             JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx,
        "setOnRawPointerEvent requires a function or null");
    auto store = detail::setWindowCallback(ctx, self, "window.rawPointer", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnRawPointerEvent(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? lcl::ui::RawPointerCallback{}
        : lcl::ui::RawPointerCallback([store](const lcl::ui::PointerEvent& event) {
            return detail::invokePointerCallback(
                store, "window.rawPointer", pointerTypeName(event.type),
                event.x, event.y, event.button, event.pointerId);
        }));
    return JS_UNDEFINED;
}

JSValue requestQuit(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    target->requestQuit(); return JS_UNDEFINED;
}

JSValue sendCancel(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    double x = 0.0, y = 0.0;
    if (argc < 2 || !numberValue(ctx, argv[0], x) || !numberValue(ctx, argv[1], y))
        return JS_ThrowTypeError(ctx, "sendPointerCancel requires x and y");
    return JS_NewBool(ctx, target->sendPointerCancel(static_cast<float>(x), static_cast<float>(y)));
}

JSValue sendScroll(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    std::array<double, 4> values{};
    if (argc < 4) return JS_ThrowTypeError(ctx, "sendPointerScroll requires x, y, deltaX, deltaY");
    for (size_t index = 0; index < values.size(); ++index)
        if (!numberValue(ctx, argv[index], values[index])) return JS_ThrowTypeError(ctx, "scroll values must be finite");
    return JS_NewBool(ctx, target->sendPointerScroll(
        static_cast<float>(values[0]), static_cast<float>(values[1]),
        static_cast<float>(values[2]), static_cast<float>(values[3])));
}

JSValue sendKeyDown(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    int32_t key = 0, codepoint = 0, modifiers = 0;
    if (argc < 1 || JS_ToInt32(ctx, &key, argv[0]) < 0) return JS_ThrowTypeError(ctx, "key code is required");
    if (argc > 1) JS_ToInt32(ctx, &codepoint, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &modifiers, argv[2]);
    return JS_NewBool(ctx, target->sendKeyDown(key, static_cast<char32_t>(std::max(0, codepoint)),
                                               static_cast<uint8_t>(modifiers)));
}
JSValue sendKeyUp(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    int32_t key = 0, modifiers = 0;
    if (argc < 1 || JS_ToInt32(ctx, &key, argv[0]) < 0) return JS_ThrowTypeError(ctx, "key code is required");
    if (argc > 1) JS_ToInt32(ctx, &modifiers, argv[1]);
    return JS_NewBool(ctx, target->sendKeyUp(key, static_cast<uint8_t>(modifiers)));
}
JSValue sendText(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    const auto text = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!text) return JS_ThrowTypeError(ctx, "text is required");
    return JS_NewBool(ctx, target->sendTextInput(*text));
}

JSValue setSystemKind(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "system surface kind is required");
    using Kind = lcl::protocol::LCLSystemSurfaceKind;
    if (*value == "none") target->setSystemSurfaceKind(Kind::None);
    else if (*value == "wallpaper") target->setSystemSurfaceKind(Kind::Wallpaper);
    else if (*value == "menu-bar" || *value == "menuBar") target->setSystemSurfaceKind(Kind::MenuBar);
    else if (*value == "dock") target->setSystemSurfaceKind(Kind::Dock);
    else if (*value == "home-screen" || *value == "homeScreen") target->setSystemSurfaceKind(Kind::HomeScreen);
    else if (*value == "permission-prompt" || *value == "permissionPrompt") target->setSystemSurfaceKind(Kind::PermissionPrompt);
    else return JS_ThrowRangeError(ctx, "unknown system surface kind");
    return JS_UNDEFINED;
}

JSValue setWindowLayer(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "window layer is required");
    lcl::protocol::LCLWindowLayer layer;
    if (*value == "bottom") layer = lcl::protocol::LCLWindowLayer::Bottom;
    else if (*value == "normal") layer = lcl::protocol::LCLWindowLayer::Normal;
    else if (*value == "top-most" || *value == "topMost") layer = lcl::protocol::LCLWindowLayer::TopMost;
    else return JS_ThrowRangeError(ctx, "unknown window layer");
    return JS_NewBool(ctx, target->setWindowLayer(layer, argc > 1 && JS_ToBool(ctx, argv[1]) != 0));
}

JSValue setReservedZone(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = window(ctx, self); if (!target) return JS_EXCEPTION;
    std::array<double, 4> values{};
    if (argc < 2) return JS_ThrowTypeError(ctx, "setReservedZone requires top and bottom");
    for (int index = 0; index < argc && index < 4; ++index)
        if (!numberValue(ctx, argv[index], values[index])) return JS_ThrowTypeError(ctx, "reserved zone values must be finite");
    return JS_NewBool(ctx, target->setReservedZone(
        static_cast<float>(values[0]), static_cast<float>(values[1]),
        static_cast<float>(values[2]), static_cast<float>(values[3])));
}

} // namespace

void registerWindowAppBindings(JSContext* ctx, JSValue proto, JSValue lcl) {
    const std::tuple<const char*, JSCFunction*, int> methods[]{
        {"setRootWidget", setRootWidget, 1}, {"runEventLoop", runEventLoop, 0},
        {"tick", tick, 0}, {"renderFrame", renderFrame, 0},
        {"setSurfaceId", setSurfaceId, 1}, {"setAppId", setAppId, 1},
        {"sendPointerMove", pointerMove, 2}, {"sendPointerDown", pointerDown, 3},
        {"sendPointerUp", pointerUp, 3}, {"requestWindowDrag", drag, 2},
        {"requestWindowMinimize", minimize, 0}, {"requestWindowMaximize", maximize, 0},
        {"requestWindowRestore", restore, 0}, {"requestWindowToggleMaximize", toggleMaximize, 0},
        {"requestWindowClose", close, 0}, {"setDecorationMode", decoration, 1},
        {"setEdgeToEdge", edgeToEdge, 1}, {"setWindowCornerStyle", cornerStyle, 2},
        {"animate", animate, 2},
        {"getWidth", getWidth, 0}, {"getHeight", getHeight, 0},
        {"getBufferScale", getBufferScale, 0}, {"getTitle", getTitle, 0},
        {"getAppId", getAppId, 0}, {"isIpcConnected", isConnected, 0},
        {"connectCompositor", connect, 1}, {"resize", resize, 2},
        {"setInitialBounds", initialBounds, 4}, {"setResizeConstraints", resizeConstraints, 1},
        {"setInputEnabled", setInputEnabled, 1}, {"isInputEnabled", getInputEnabled, 0},
        {"setOnResize", setOnResize, 1}, {"setOnFrame", setOnFrame, 1},
        {"setOnRawPointerEvent", setOnRawPointerEvent, 1},
        {"requestQuit", requestQuit, 0}, {"sendPointerCancel", sendCancel, 2},
        {"sendPointerScroll", sendScroll, 4}, {"sendKeyDown", sendKeyDown, 3},
        {"sendKeyUp", sendKeyUp, 2}, {"sendTextInput", sendText, 1},
        {"setSystemSurfaceKind", setSystemKind, 1}, {"setWindowLayer", setWindowLayer, 2},
        {"setReservedZone", setReservedZone, 4},
    };
    for (const auto& [name, function, argc] : methods)
        detail::addMethod(ctx, proto, name, function, argc);
    detail::addConstructor(ctx, lcl, "WindowApp", constructor, 3, proto);
}

} // namespace lcl::binding
