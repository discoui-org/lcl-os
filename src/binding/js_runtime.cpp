#include "binding/js_runtime.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/core/animation.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/image.hpp"
#include "render/skia_canvas.hpp"

namespace lcl::binding {

namespace {

// QuickJS Class IDs
JSClassID g_window_app_class_id = 0;
JSClassID g_widget_class_id = 0;
JSClassID g_animation_engine_class_id = 0;
JSClassID g_animation_handle_class_id = 0;

struct JsWindowAppWrapper {
    lcl::ui::WindowApp* app{nullptr};
};

struct JsWidgetWrapper {
    lcl::ui::Widget* widget{nullptr};
    bool ownedByCpp{false};
};

struct JsAnimationEngineWrapper {
    lcl::ui::AnimationEngine* engine{nullptr};
};

struct JsAnimationHandleWrapper {
    JSContext* ctx{nullptr};
    std::vector<lcl::motion::AnimationHandle> handles;
    JSValue completion{JS_UNDEFINED};
    size_t completedCount{0};
    bool completionCalled{false};
};

JsWidgetWrapper* makeWidgetWrapper(lcl::ui::Widget* widget) {
    auto* wrapper = new JsWidgetWrapper();
    wrapper->widget = widget;
    if (widget) {
        widget->setDestructionCallback([wrapper] { wrapper->widget = nullptr; });
    }
    return wrapper;
}

// Finalizers
void js_window_app_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    auto* wrapper = static_cast<JsWindowAppWrapper*>(JS_GetOpaque(val, g_window_app_class_id));
    if (wrapper) {
        if (wrapper->app) {
            delete wrapper->app;
            wrapper->app = nullptr;
        }
        delete wrapper;
    }
}

void js_widget_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    auto* wrapper = static_cast<JsWidgetWrapper*>(JS_GetOpaque(val, g_widget_class_id));
    if (wrapper) {
        if (wrapper->widget) wrapper->widget->setDestructionCallback(nullptr);
        if (!wrapper->ownedByCpp && wrapper->widget) {
            delete wrapper->widget;
            wrapper->widget = nullptr;
        }
        delete wrapper;
    }
}

void js_animation_engine_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    auto* wrapper = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque(val, g_animation_engine_class_id));
    if (wrapper) {
        if (wrapper->engine) {
            delete wrapper->engine;
            wrapper->engine = nullptr;
        }
        delete wrapper;
    }
}

void js_animation_handle_finalizer(JSRuntime* rt, JSValue val) {
    auto* wrapper = static_cast<JsAnimationHandleWrapper*>(JS_GetOpaque(val, g_animation_handle_class_id));
    if (!wrapper) return;
    if (!JS_IsUndefined(wrapper->completion)) JS_FreeValueRT(rt, wrapper->completion);
    delete wrapper;
}

void js_animation_handle_gc_mark(JSRuntime* rt, JSValue val, JS_MarkFunc* mark_func) {
    auto* wrapper = static_cast<JsAnimationHandleWrapper*>(JS_GetOpaque(val, g_animation_handle_class_id));
    if (wrapper && !JS_IsUndefined(wrapper->completion)) JS_MarkValue(rt, wrapper->completion, mark_func);
}

void mark_widget_gc(lcl::ui::Widget* w, JSRuntime* rt, JS_MarkFunc* mark_func) {
    if (!w) return;
    if (w->getGcMarkCallback()) {
        w->getGcMarkCallback()(reinterpret_cast<void*>(rt), reinterpret_cast<void*>(mark_func));
    }
    for (const auto& child : w->getChildren()) {
        mark_widget_gc(child.get(), rt, mark_func);
    }
}

void js_window_app_gc_mark(JSRuntime* rt, JSValue val, JS_MarkFunc* mark_func) {
    auto* wrapper = static_cast<JsWindowAppWrapper*>(JS_GetOpaque(val, g_window_app_class_id));
    if (wrapper && wrapper->app && wrapper->app->getRootWidget()) {
        mark_widget_gc(wrapper->app->getRootWidget(), rt, mark_func);
    }
}

static JSClassDef js_window_app_class = {
    "WindowApp",
    js_window_app_finalizer,
    js_window_app_gc_mark,
    nullptr,
    nullptr
};

static JSClassDef js_widget_class = {
    "Widget",
    js_widget_finalizer,
    nullptr,
    nullptr,
    nullptr
};

static JSClassDef js_animation_engine_class = {
    "AnimationEngine",
    js_animation_engine_finalizer,
    nullptr,
    nullptr,
    nullptr
};

static JSClassDef js_animation_handle_class = {
    "AnimationHandle",
    js_animation_handle_finalizer,
    js_animation_handle_gc_mark,
    nullptr,
    nullptr
};

lcl::ui::EasingName easingFromString(const std::string& name) {
    using lcl::ui::EasingName;
    if (name == "linear") return EasingName::Linear;
    if (name == "easeInSine") return EasingName::EaseInSine;
    if (name == "easeOutSine") return EasingName::EaseOutSine;
    if (name == "easeInOutSine") return EasingName::EaseInOutSine;
    if (name == "easeInQuad") return EasingName::EaseInQuad;
    if (name == "easeOutQuad") return EasingName::EaseOutQuad;
    if (name == "easeInOutQuad") return EasingName::EaseInOutQuad;
    if (name == "easeInCubic") return EasingName::EaseInCubic;
    if (name == "easeOutCubic") return EasingName::EaseOutCubic;
    if (name == "easeInOutCubic") return EasingName::EaseInOutCubic;
    if (name == "easeInQuart") return EasingName::EaseInQuart;
    if (name == "easeOutQuart") return EasingName::EaseOutQuart;
    if (name == "easeInOutQuart") return EasingName::EaseInOutQuart;
    if (name == "easeInQuint") return EasingName::EaseInQuint;
    if (name == "easeOutQuint") return EasingName::EaseOutQuint;
    if (name == "easeInOutQuint") return EasingName::EaseInOutQuint;
    if (name == "easeInExpo") return EasingName::EaseInExpo;
    if (name == "easeOutExpo") return EasingName::EaseOutExpo;
    if (name == "easeInOutExpo") return EasingName::EaseInOutExpo;
    if (name == "easeInCirc") return EasingName::EaseInCirc;
    if (name == "easeOutCirc") return EasingName::EaseOutCirc;
    if (name == "easeInOutCirc") return EasingName::EaseInOutCirc;
    if (name == "easeInBack") return EasingName::EaseInBack;
    if (name == "easeOutBack") return EasingName::EaseOutBack;
    if (name == "easeInOutBack") return EasingName::EaseInOutBack;
    if (name == "easeInElastic") return EasingName::EaseInElastic;
    if (name == "easeOutElastic") return EasingName::EaseOutElastic;
    if (name == "easeInOutElastic") return EasingName::EaseInOutElastic;
    if (name == "easeInBounce") return EasingName::EaseInBounce;
    if (name == "easeOutBounce") return EasingName::EaseOutBounce;
    if (name == "easeInOutBounce") return EasingName::EaseInOutBounce;
    return EasingName::EaseOutCubic;
}

lcl::motion::Easing parseEasing(const std::string& name) {
    float x1 = 0.0f, y1 = 0.0f, x2 = 1.0f, y2 = 1.0f;
    if (std::sscanf(name.c_str(), "cubic-bezier(%f,%f,%f,%f)", &x1, &y1, &x2, &y2) == 4)
        return lcl::motion::Easing::cubicBezier(x1, y1, x2, y2);
    unsigned count = 0;
    char position[16]{};
    if (std::sscanf(name.c_str(), "steps(%u,%15[^)])", &count, position) == 2)
        return lcl::motion::Easing::steps(count,
            std::string(position) == "start" ? lcl::motion::StepPosition::Start : lcl::motion::StepPosition::End);
    return lcl::motion::Easing(easingFromString(name));
}

std::string stringProperty(JSContext* ctx, JSValueConst object, const char* name,
                           const std::string& fallback) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    const char* text = JS_ToCString(ctx, value);
    std::string result = text ? text : fallback;
    if (text) JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, value);
    return result;
}

double numberProperty(JSContext* ctx, JSValueConst object, const char* name, double fallback) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    double result = fallback;
    if (JS_ToFloat64(ctx, &result, value) < 0) result = fallback;
    JS_FreeValue(ctx, value);
    return result;
}

JSValue js_window_app_animate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!wrap || !wrap->app) return JS_EXCEPTION;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "animate(options, callback) requires an options object and function");

    const std::string type = stringProperty(ctx, argv[0], "type", "spring");
    const float duration = static_cast<float>(std::max(0.0, numberProperty(ctx, argv[0], "duration", 180.0)) / 1000.0);
    lcl::motion::Motion motion;
    try {
        if (type == "tween") {
            const float delay = static_cast<float>(std::max(0.0, numberProperty(ctx, argv[0], "delay", 0.0)) / 1000.0);
            motion = lcl::motion::Motion::tween(duration,
                parseEasing(stringProperty(ctx, argv[0], "easing", "easeOutCubic")), delay);
        } else {
            const double stiffness = numberProperty(ctx, argv[0], "stiffness", -1.0);
            if (stiffness > 0.0) {
                motion = lcl::motion::Motion::spring(
                    static_cast<float>(numberProperty(ctx, argv[0], "mass", 1.0)),
                    static_cast<float>(stiffness),
                    static_cast<float>(numberProperty(ctx, argv[0], "damping", 36.0)),
                    static_cast<float>(numberProperty(ctx, argv[0], "initialVelocity", 0.0)));
            } else {
                motion = lcl::motion::Motion::spring(duration,
                    static_cast<float>(numberProperty(ctx, argv[0], "bounce", 0.0)));
            }
        }
    } catch (const std::exception& error) {
        return JS_ThrowRangeError(ctx, "%s", error.what());
    }

    lcl::ui::AnimationTransactionOptions options;
    options.layout = stringProperty(ctx, argv[0], "layout", "reflow") == "morph"
        ? lcl::ui::LayoutMode::Morph : lcl::ui::LayoutMode::Reflow;
    JSValue callbackResult = JS_UNDEFINED;
    wrap->app->animate(motion, options, [&] {
        callbackResult = JS_Call(ctx, argv[1], JS_UNDEFINED, 0, nullptr);
    });
    if (JS_IsException(callbackResult)) return callbackResult;
    JS_FreeValue(ctx, callbackResult);
    return JS_UNDEFINED;
}

// ------------------------------------------------------------
// console.log Implementation
// ------------------------------------------------------------
JSValue js_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    std::string msg = "[JS console] ";
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            msg += str;
            JS_FreeCString(ctx, str);
        }
    }
    std::cout << msg << std::endl;
    return JS_UNDEFINED;
}

// ------------------------------------------------------------
// WindowApp Methods
// ------------------------------------------------------------
JSValue js_window_app_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target;
    int width = 800;
    int height = 600;
    std::string title = "LCL JS App";

    if (argc >= 1) JS_ToInt32(ctx, &width, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &height, argv[1]);
    if (argc >= 3) {
        const char* str = JS_ToCString(ctx, argv[2]);
        if (str) { title = str; JS_FreeCString(ctx, str); }
    }

    JSValue obj = JS_NewObjectClass(ctx, g_window_app_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = new JsWindowAppWrapper();
    wrapper->app = new lcl::ui::WindowApp(
        lcl::render::makeSkiaCanvas(), width, height, title);
    JS_SetOpaque(obj, wrapper);
    return obj;
}

JSValue js_window_app_setRootWidget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    if (argc >= 1) {
        auto* widgetWrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque(argv[0], g_widget_class_id));
        if (widgetWrap && widgetWrap->widget && !widgetWrap->ownedByCpp) {
            appWrap->app->setRootWidget(std::unique_ptr<lcl::ui::Widget>(widgetWrap->widget));
            widgetWrap->ownedByCpp = true;
        }
    }
    return JS_UNDEFINED;
}

JSValue js_window_app_connectCompositor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    bool connected = appWrap->app->connectCompositor();
    return JS_NewBool(ctx, connected);
}

JSValue js_window_app_runEventLoop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    appWrap->app->runEventLoop();
    return JS_UNDEFINED;
}

JSValue js_window_app_renderFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    bool rendered = appWrap->app->renderFrame();
    return JS_NewBool(ctx, rendered);
}

JSValue js_window_app_tick(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    return JS_NewBool(ctx, appWrap->app->tick());
}

JSValue js_window_app_setSurfaceId(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    int32_t surfaceId = 0;
    if (argc < 1 || JS_ToInt32(ctx, &surfaceId, argv[0]) < 0 || surfaceId <= 0) {
        return JS_ThrowRangeError(ctx, "surfaceId must be a positive integer");
    }

    appWrap->app->setSurfaceId(static_cast<uint32_t>(surfaceId));
    // setSurfaceId intentionally becomes immutable once connected.
    return JS_NewBool(ctx, appWrap->app->getSurfaceId() == static_cast<uint32_t>(surfaceId));
}

JSValue js_window_app_setAppId(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "appId is required");
    const char* value = JS_ToCString(ctx, argv[0]);
    if (!value) return JS_EXCEPTION;
    std::string appId(value);
    JS_FreeCString(ctx, value);
    if (appId.empty() || appId.size() >= 64) {
        return JS_ThrowRangeError(ctx, "appId must contain 1..63 bytes");
    }
    appWrap->app->setAppId(std::move(appId));
    return JS_UNDEFINED;
}

JSValue js_window_app_sendPointerMove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    double x = 0.0, y = 0.0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);

    bool handled = appWrap->app->sendPointerMove(static_cast<float>(x), static_cast<float>(y));
    return JS_NewBool(ctx, handled);
}

JSValue js_window_app_sendPointerDown(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    double x = 0.0, y = 0.0;
    int button = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    bool handled = appWrap->app->sendPointerDown(static_cast<float>(x), static_cast<float>(y), button);
    return JS_NewBool(ctx, handled);
}

JSValue js_window_app_sendPointerUp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    double x = 0.0, y = 0.0;
    int button = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &button, argv[2]);

    bool handled = appWrap->app->sendPointerUp(static_cast<float>(x), static_cast<float>(y), button);
    return JS_NewBool(ctx, handled);
}

JSValue js_window_app_requestWindowDrag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    double x = 0.0;
    double y = 0.0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);

    bool ok = appWrap->app->requestWindowDrag(static_cast<float>(x), static_cast<float>(y));
    return JS_NewBool(ctx, ok);
}

JSValue js_window_app_requestWindowMinimize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;
    return JS_NewBool(ctx, appWrap->app->requestWindowMinimize());
}

JSValue js_window_app_requestWindowMaximize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;
    return JS_NewBool(ctx, appWrap->app->requestWindowMaximize());
}

JSValue js_window_app_requestWindowRestore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;
    return JS_NewBool(ctx, appWrap->app->requestWindowRestore());
}

JSValue js_window_app_requestWindowToggleMaximize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;
    return JS_NewBool(ctx, appWrap->app->requestWindowToggleMaximize());
}

JSValue js_window_app_requestWindowClose(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    bool ok = appWrap->app->requestWindowClose();
    return JS_NewBool(ctx, ok);
}

JSValue js_window_app_setCsdTitlebarEnabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    int enabled = 0;
    if (argc >= 1) {
        enabled = JS_ToBool(ctx, argv[0]);
    }
    appWrap->app->setCsdTitlebarEnabled(enabled != 0);
    return JS_UNDEFINED;
}

JSValue js_window_app_configureCsdTitlebar(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    double height = 32.0;
    double controlLeft = 10.0;
    double controlTop = 8.0;
    double controlSize = 16.0;
    double controlGap = 6.0;

    if (argc >= 1) JS_ToFloat64(ctx, &height, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &controlLeft, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &controlTop, argv[2]);
    if (argc >= 4) JS_ToFloat64(ctx, &controlSize, argv[3]);
    if (argc >= 5) JS_ToFloat64(ctx, &controlGap, argv[4]);

    appWrap->app->configureCsdTitlebar(
        static_cast<float>(height),
        static_cast<float>(controlLeft),
        static_cast<float>(controlTop),
        static_cast<float>(controlSize),
        static_cast<float>(controlGap));
    return JS_UNDEFINED;
}

JSValue js_window_app_setDecorationMode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* appWrap = static_cast<JsWindowAppWrapper*>(JS_GetOpaque2(ctx, this_val, g_window_app_class_id));
    if (!appWrap || !appWrap->app) return JS_EXCEPTION;

    std::string mode = "ssd";
    if (argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) {
            mode = str;
            JS_FreeCString(ctx, str);
        }
    }

    lcl::protocol::LCLDecorationMode value = lcl::protocol::LCLDecorationMode::SSD;
    if (mode == "csd") {
        value = lcl::protocol::LCLDecorationMode::CSD;
    } else if (mode == "none" || mode == "frameless") {
        value = lcl::protocol::LCLDecorationMode::None;
    }

    bool ok = appWrap->app->setDecorationMode(value);
    return JS_NewBool(ctx, ok);
}

// ------------------------------------------------------------
// Widget / Container / Button / Text Constructors
// ------------------------------------------------------------
JSValue js_container_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target; (void)argc; (void)argv;
    JSValue obj = JS_NewObjectClass(ctx, g_widget_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = makeWidgetWrapper(new lcl::ui::Container());
    JS_SetOpaque(obj, wrapper);
    return obj;
}

JSValue js_button_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target;
    std::string label = "";
    if (argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) { label = str; JS_FreeCString(ctx, str); }
    }

    JSValue obj = JS_NewObjectClass(ctx, g_widget_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = makeWidgetWrapper(new lcl::ui::Button(label));
    JS_SetOpaque(obj, wrapper);
    return obj;
}

JSValue js_backdrop_surface_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target; (void)argc; (void)argv;
    JSValue obj = JS_NewObjectClass(ctx, g_widget_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = makeWidgetWrapper(new lcl::ui::BackdropSurface());
    JS_SetOpaque(obj, wrapper);
    return obj;
}

JSValue js_text_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target;
    std::string text = "";
    if (argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) { text = str; JS_FreeCString(ctx, str); }
    }

    JSValue obj = JS_NewObjectClass(ctx, g_widget_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = makeWidgetWrapper(new lcl::ui::Text(text));
    JS_SetOpaque(obj, wrapper);
    return obj;
}

JSValue js_image_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target;
    std::string sourcePath = "";
    if (argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) { sourcePath = str; JS_FreeCString(ctx, str); }
    }

    JSValue obj = JS_NewObjectClass(ctx, g_widget_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = makeWidgetWrapper(new lcl::ui::Image(sourcePath));
    JS_SetOpaque(obj, wrapper);
    return obj;
}

// ------------------------------------------------------------
// Common Widget Methods
// ------------------------------------------------------------
JSValue js_widget_setWidth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        double val = 0.0;
        JS_ToFloat64(ctx, &val, argv[0]);
        wrap->widget->setWidth(static_cast<float>(val));
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setHeight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        double val = 0.0;
        JS_ToFloat64(ctx, &val, argv[0]);
        wrap->widget->setHeight(static_cast<float>(val));
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setDirection(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) {
            std::string d = str;
            JS_FreeCString(ctx, str);
            if (d == "row") {
                wrap->widget->getYogaNode().setDirection(YGFlexDirectionRow);
            } else {
                wrap->widget->getYogaNode().setDirection(YGFlexDirectionColumn);
            }
        }
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setJustifyContent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) {
            std::string j = str;
            JS_FreeCString(ctx, str);
            if (j == "center") wrap->widget->getYogaNode().setJustifyContent(YGJustifyCenter);
            else if (j == "flex-start" || j == "start") wrap->widget->getYogaNode().setJustifyContent(YGJustifyFlexStart);
            else if (j == "flex-end" || j == "end") wrap->widget->getYogaNode().setJustifyContent(YGJustifyFlexEnd);
            else if (j == "space-between") wrap->widget->getYogaNode().setJustifyContent(YGJustifySpaceBetween);
            else if (j == "space-around") wrap->widget->getYogaNode().setJustifyContent(YGJustifySpaceAround);
            else if (j == "space-evenly") wrap->widget->getYogaNode().setJustifyContent(YGJustifySpaceEvenly);
        }
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setAlignItems(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) {
            std::string a = str;
            JS_FreeCString(ctx, str);
            if (a == "center") wrap->widget->getYogaNode().setAlignItems(YGAlignCenter);
            else if (a == "flex-start" || a == "start") wrap->widget->getYogaNode().setAlignItems(YGAlignFlexStart);
            else if (a == "flex-end" || a == "end") wrap->widget->getYogaNode().setAlignItems(YGAlignFlexEnd);
            else if (a == "stretch") wrap->widget->getYogaNode().setAlignItems(YGAlignStretch);
        }
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setPadding(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        double val = 0.0;
        JS_ToFloat64(ctx, &val, argv[0]);
        wrap->widget->setPadding(YGEdgeAll, static_cast<float>(val));
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setGap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        double val = 0.0;
        JS_ToFloat64(ctx, &val, argv[0]);
        wrap->widget->setGap(YGGutterAll, static_cast<float>(val));
    }
    return JS_UNDEFINED;
}

JSValue js_widget_addChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* parentWrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!parentWrap || !parentWrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        auto* childWrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque(argv[0], g_widget_class_id));
        if (childWrap && childWrap->widget && !childWrap->ownedByCpp) {
            parentWrap->widget->addChild(std::unique_ptr<lcl::ui::Widget>(childWrap->widget));
            childWrap->ownedByCpp = true;
        }
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setBackgroundColor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* container = dynamic_cast<lcl::ui::Container*>(wrap->widget);
    if (!container || argc < 3) return JS_UNDEFINED;

    int r = 0, g = 0, b = 0, a = 255;
    JS_ToInt32(ctx, &r, argv[0]);
    JS_ToInt32(ctx, &g, argv[1]);
    JS_ToInt32(ctx, &b, argv[2]);
    if (argc >= 4) JS_ToInt32(ctx, &a, argv[3]);

    container->setBackgroundColor(lcl::ui::Color{
        static_cast<uint8_t>(std::clamp(r, 0, 255)),
        static_cast<uint8_t>(std::clamp(g, 0, 255)),
        static_cast<uint8_t>(std::clamp(b, 0, 255)),
        static_cast<uint8_t>(std::clamp(a, 0, 255))
    });
    return JS_UNDEFINED;
}

JSValue js_widget_setBorderColor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* container = dynamic_cast<lcl::ui::Container*>(wrap->widget);
    if (!container || argc < 3) return JS_UNDEFINED;

    int r = 0, g = 0, b = 0, a = 255;
    JS_ToInt32(ctx, &r, argv[0]);
    JS_ToInt32(ctx, &g, argv[1]);
    JS_ToInt32(ctx, &b, argv[2]);
    if (argc >= 4) JS_ToInt32(ctx, &a, argv[3]);

    container->setBorderColor(lcl::ui::Color{
        static_cast<uint8_t>(std::clamp(r, 0, 255)),
        static_cast<uint8_t>(std::clamp(g, 0, 255)),
        static_cast<uint8_t>(std::clamp(b, 0, 255)),
        static_cast<uint8_t>(std::clamp(a, 0, 255))
    });
    return JS_UNDEFINED;
}

JSValue js_widget_setBorderWidth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* container = dynamic_cast<lcl::ui::Container*>(wrap->widget);
    if (!container || argc < 1) return JS_UNDEFINED;

    double width = 0.0;
    JS_ToFloat64(ctx, &width, argv[0]);
    container->setBorderWidth(static_cast<float>(std::max(0.0, width)));
    return JS_UNDEFINED;
}

JSValue js_widget_setBorderRadius(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* container = dynamic_cast<lcl::ui::Container*>(wrap->widget);
    if (!container || argc < 1) return JS_UNDEFINED;

    double radius = 0.0;
    JS_ToFloat64(ctx, &radius, argv[0]);
    container->setBorderRadius(static_cast<float>(std::max(0.0, radius)));

    if (argc >= 2) {
        double roundness = 2.0;
        JS_ToFloat64(ctx, &roundness, argv[1]);
        container->setBorderRoundness(static_cast<float>(std::clamp(roundness, 2.0, 8.0)));
    }

    return JS_UNDEFINED;
}

JSValue js_widget_setBorderRoundness(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* container = dynamic_cast<lcl::ui::Container*>(wrap->widget);
    if (!container || argc < 1) return JS_UNDEFINED;

    double roundness = 2.0;
    JS_ToFloat64(ctx, &roundness, argv[0]);
    container->setBorderRoundness(static_cast<float>(std::clamp(roundness, 2.0, 8.0)));
    return JS_UNDEFINED;
}

JSValue js_text_setTextColor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* txt = dynamic_cast<lcl::ui::Text*>(wrap->widget);
    if (!txt || argc < 3) return JS_UNDEFINED;

    int r = 0, g = 0, b = 0, a = 255;
    JS_ToInt32(ctx, &r, argv[0]);
    JS_ToInt32(ctx, &g, argv[1]);
    JS_ToInt32(ctx, &b, argv[2]);
    if (argc >= 4) JS_ToInt32(ctx, &a, argv[3]);

    txt->setTextColor(lcl::ui::Color{
        static_cast<uint8_t>(std::clamp(r, 0, 255)),
        static_cast<uint8_t>(std::clamp(g, 0, 255)),
        static_cast<uint8_t>(std::clamp(b, 0, 255)),
        static_cast<uint8_t>(std::clamp(a, 0, 255))
    });
    return JS_UNDEFINED;
}

JSValue js_effect_addFilter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* blurSurface = dynamic_cast<lcl::ui::BackdropSurface*>(wrap->widget);
    if (!blurSurface || argc < 2) return JS_UNDEFINED;

    const char* filterName = JS_ToCString(ctx, argv[0]);
    if (!filterName) return JS_UNDEFINED;

    double value = 0.0;
    JS_ToFloat64(ctx, &value, argv[1]);

    std::string name = filterName;
    JS_FreeCString(ctx, filterName);

    lcl::protocol::FilterType filterType = lcl::protocol::FilterType::None;
    if (name == "blur") filterType = lcl::protocol::FilterType::Blur;
    else if (name == "glass") filterType = lcl::protocol::FilterType::Glass;
    else if (name == "brightness") filterType = lcl::protocol::FilterType::Brightness;
    else if (name == "contrast") filterType = lcl::protocol::FilterType::Contrast;
    else if (name == "saturation") filterType = lcl::protocol::FilterType::Saturation;
    else if (name == "grayscale") filterType = lcl::protocol::FilterType::Grayscale;
    else if (name == "invert") filterType = lcl::protocol::FilterType::Invert;

    if (filterType != lcl::protocol::FilterType::None) {
        if (filterType == lcl::protocol::FilterType::Glass) {
            double refractionFactor = 1.4;
            double dispersionGain = 7.0;
            if (argc >= 3) JS_ToFloat64(ctx, &refractionFactor, argv[2]);
            if (argc >= 4) JS_ToFloat64(ctx, &dispersionGain, argv[3]);
            blurSurface->setGlass(
                static_cast<float>(std::max(0.0, value)),
                static_cast<float>(std::max(1.0, refractionFactor)),
                static_cast<float>(std::max(0.0, dispersionGain)));
        } else {
            blurSurface->addFilter(filterType, static_cast<float>(value));
        }
    }
    return JS_UNDEFINED;
}

JSValue js_effect_setGlass(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* blurSurface = dynamic_cast<lcl::ui::BackdropSurface*>(wrap->widget);
    if (!blurSurface || argc < 1) return JS_UNDEFINED;

    double thicknessPx = 20.0;
    double refractionFactor = 1.4;
    double dispersionGain = 7.0;

    if (argc >= 1) JS_ToFloat64(ctx, &thicknessPx, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &refractionFactor, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &dispersionGain, argv[2]);

    blurSurface->setGlass(
        static_cast<float>(std::max(0.0, thicknessPx)),
        static_cast<float>(std::max(1.0, refractionFactor)),
        static_cast<float>(std::max(0.0, dispersionGain)));
    return JS_UNDEFINED;
}

JSValue js_effect_clearFilters(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* blurSurface = dynamic_cast<lcl::ui::BackdropSurface*>(wrap->widget);
    if (blurSurface) {
        blurSurface->clearFilters();
    }
    return JS_UNDEFINED;
}

JSValue js_backdrop_setInteractive(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* backdrop = dynamic_cast<lcl::ui::BackdropSurface*>(wrap->widget);
    if (!backdrop || argc < 1) return JS_UNDEFINED;
    backdrop->setInteractive(JS_ToBool(ctx, argv[0]) != 0);
    return JS_UNDEFINED;
}

JSValue js_effect_setOpacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc < 1) return JS_UNDEFINED;

    double opacity = 1.0;
    JS_ToFloat64(ctx, &opacity, argv[0]);
    const float clamped = static_cast<float>(std::clamp(opacity, 0.0, 1.0));

    wrap->widget->setOpacity(clamped);

    return JS_UNDEFINED;
}

JSValue js_widget_setTranslation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;
    double x = 0.0, y = 0.0;
    if (argc >= 1 && JS_ToFloat64(ctx, &x, argv[0]) < 0) return JS_EXCEPTION;
    if (argc >= 2 && JS_ToFloat64(ctx, &y, argv[1]) < 0) return JS_EXCEPTION;
    wrap->widget->setTranslation(static_cast<float>(x), static_cast<float>(y));
    return JS_UNDEFINED;
}

JSValue js_widget_setScale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;
    double x = 1.0, y = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &x, argv[0]) < 0) return JS_ThrowTypeError(ctx, "scale is required");
    y = x;
    if (argc >= 2 && JS_ToFloat64(ctx, &y, argv[1]) < 0) return JS_EXCEPTION;
    wrap->widget->setScale(static_cast<float>(x), static_cast<float>(y));
    return JS_UNDEFINED;
}

JSValue js_widget_setRotation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;
    double value = 0.0;
    if (argc < 1 || JS_ToFloat64(ctx, &value, argv[0]) < 0) return JS_ThrowTypeError(ctx, "rotation is required");
    wrap->widget->setRotation(static_cast<float>(value));
    return JS_UNDEFINED;
}

JSValue js_widget_setTransformOrigin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;
    double x = 0.5, y = 0.5;
    if (argc < 2 || JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0)
        return JS_ThrowTypeError(ctx, "transform origin x and y are required");
    wrap->widget->setTransformOrigin(static_cast<float>(x), static_cast<float>(y));
    return JS_UNDEFINED;
}

JSValue js_image_setSourcePath(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* image = dynamic_cast<lcl::ui::Image*>(wrap->widget);
    if (!image || argc < 1) return JS_NewBool(ctx, false);

    const char* str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_NewBool(ctx, false);
    const bool ok = image->setSourcePath(str);
    JS_FreeCString(ctx, str);
    return JS_NewBool(ctx, ok);
}

JSValue js_image_clearSource(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* image = dynamic_cast<lcl::ui::Image*>(wrap->widget);
    if (image) {
        image->clearSource();
    }
    return JS_UNDEFINED;
}

JSValue js_image_setFit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* image = dynamic_cast<lcl::ui::Image*>(wrap->widget);
    if (!image || argc < 1) return JS_UNDEFINED;

    const char* str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_UNDEFINED;

    std::string fit = str;
    JS_FreeCString(ctx, str);

    if (fit == "fill") {
        image->setFit(lcl::ui::ImageFit::Fill);
    } else {
        image->setFit(lcl::ui::ImageFit::Contain);
    }

    return JS_UNDEFINED;
}

JSValue js_image_setCornerRadius(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* image = dynamic_cast<lcl::ui::Image*>(wrap->widget);
    if (!image || argc < 1) return JS_UNDEFINED;

    double radius = 0.0;
    JS_ToFloat64(ctx, &radius, argv[0]);
    image->setCornerRadius(static_cast<float>(std::max(0.0, radius)));

    if (argc >= 2) {
        double roundness = 2.0;
        JS_ToFloat64(ctx, &roundness, argv[1]);
        image->setCornerRoundness(static_cast<float>(std::clamp(roundness, 2.0, 8.0)));
    }

    return JS_UNDEFINED;
}

JSValue js_image_setCornerRoundness(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* image = dynamic_cast<lcl::ui::Image*>(wrap->widget);
    if (!image || argc < 1) return JS_UNDEFINED;

    double roundness = 2.0;
    JS_ToFloat64(ctx, &roundness, argv[0]);
    image->setCornerRoundness(static_cast<float>(std::clamp(roundness, 2.0, 8.0)));
    return JS_UNDEFINED;
}

// ------------------------------------------------------------
// Widget keyframe animations / AnimationHandle
// ------------------------------------------------------------
std::optional<lcl::ui::AnimatableProperty> propertyFromString(const std::string& name) {
    using P = lcl::ui::AnimatableProperty;
    if (name == "opacity") return P::Opacity;
    if (name == "translationX" || name == "translateX") return P::TranslationX;
    if (name == "translationY" || name == "translateY") return P::TranslationY;
    if (name == "scaleX") return P::ScaleX;
    if (name == "scaleY") return P::ScaleY;
    if (name == "rotation") return P::Rotation;
    if (name == "width") return P::Width;
    if (name == "height") return P::Height;
    if (name == "borderWidth") return P::BorderWidth;
    if (name == "borderRadius") return P::BorderRadius;
    return std::nullopt;
}

JSValue js_widget_animate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* widgetWrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!widgetWrap || !widgetWrap->widget) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "animate requires a keyframe array");
    if (!widgetWrap->widget->getMotionCoordinator())
        return JS_ThrowTypeError(ctx, "widget must be mounted in a WindowApp before animate()");

    JSValue lengthValue = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t frameCount = 0;
    if (JS_ToUint32(ctx, &frameCount, lengthValue) < 0 || frameCount == 0) {
        JS_FreeValue(ctx, lengthValue);
        return JS_ThrowRangeError(ctx, "animate requires at least one keyframe");
    }
    JS_FreeValue(ctx, lengthValue);

    lcl::motion::AnimationOptions options;
    JSValueConst optionValue = argc >= 2 ? argv[1] : JS_UNDEFINED;
    try {
        if (JS_IsNumber(optionValue)) {
            double duration = 180.0;
            JS_ToFloat64(ctx, &duration, optionValue);
            options.durationSec = static_cast<float>(std::max(0.0, duration) / 1000.0);
        } else if (JS_IsObject(optionValue)) {
            options.durationSec = static_cast<float>(std::max(0.0, numberProperty(ctx, optionValue, "duration", 180.0)) / 1000.0);
            options.delaySec = static_cast<float>(std::max(0.0, numberProperty(ctx, optionValue, "delay", 0.0)) / 1000.0);
            const double iterations = std::clamp(numberProperty(ctx, optionValue, "iterations", 1.0), 1.0, 10000.0);
            options.repeat = static_cast<uint32_t>(iterations - 1.0);
            options.easing = parseEasing(stringProperty(ctx, optionValue, "easing", "linear"));
            options.fill = stringProperty(ctx, optionValue, "fill", "none") == "forwards"
                ? lcl::motion::FillMode::Forwards : lcl::motion::FillMode::None;
            const std::string direction = stringProperty(ctx, optionValue, "direction", "normal");
            if (direction == "reverse") options.direction = lcl::motion::PlaybackDirection::Reverse;
            else if (direction == "alternate") options.direction = lcl::motion::PlaybackDirection::Alternate;
            else if (direction == "alternate-reverse") options.direction = lcl::motion::PlaybackDirection::AlternateReverse;
        }
    } catch (const std::exception& error) {
        return JS_ThrowRangeError(ctx, "%s", error.what());
    }

    const std::array<std::string, 11> names{
        "opacity", "translationX", "translationY", "scaleX", "scaleY", "rotation",
        "width", "height", "borderWidth", "borderRadius", "scale"};
    auto* handleWrap = new JsAnimationHandleWrapper();
    handleWrap->ctx = ctx;

    try {
        for (const auto& name : names) {
            std::vector<lcl::motion::Keyframe> frames;
            for (uint32_t index = 0; index < frameCount; ++index) {
                JSValue frame = JS_GetPropertyUint32(ctx, argv[0], index);
                JSValue value = JS_GetPropertyStr(ctx, frame, name.c_str());
                if (!JS_IsUndefined(value)) {
                    double number = 0.0;
                    if (JS_ToFloat64(ctx, &number, value) < 0) {
                        JS_FreeValue(ctx, value); JS_FreeValue(ctx, frame); delete handleWrap;
                        return JS_ThrowTypeError(ctx, "keyframe values must be numbers");
                    }
                    const float fallbackOffset = frameCount == 1 ? 1.0f : static_cast<float>(index) / static_cast<float>(frameCount - 1);
                    const float offset = static_cast<float>(numberProperty(ctx, frame, "offset", fallbackOffset));
                    frames.push_back({offset, static_cast<float>(number)});
                }
                JS_FreeValue(ctx, value);
                JS_FreeValue(ctx, frame);
            }
            if (frames.empty()) continue;
            if (name == "scale") {
                handleWrap->handles.push_back(widgetWrap->widget->animate(lcl::ui::AnimatableProperty::ScaleX, frames, options));
                handleWrap->handles.push_back(widgetWrap->widget->animate(lcl::ui::AnimatableProperty::ScaleY, std::move(frames), options));
            } else if (const auto property = propertyFromString(name)) {
                handleWrap->handles.push_back(widgetWrap->widget->animate(*property, std::move(frames), options));
            }
        }
    } catch (const std::exception& error) {
        delete handleWrap;
        return JS_ThrowRangeError(ctx, "%s", error.what());
    }

    if (handleWrap->handles.empty()) {
        delete handleWrap;
        return JS_ThrowTypeError(ctx, "keyframes do not contain an animatable property");
    }
    JSValue result = JS_NewObjectClass(ctx, g_animation_handle_class_id);
    if (JS_IsException(result)) { delete handleWrap; return result; }
    JS_SetOpaque(result, handleWrap);
    return result;
}

template <typename Callback>
JSValue withAnimationHandles(JSContext* ctx, JSValueConst thisVal, Callback callback) {
    auto* wrapper = static_cast<JsAnimationHandleWrapper*>(JS_GetOpaque2(ctx, thisVal, g_animation_handle_class_id));
    if (!wrapper) return JS_EXCEPTION;
    for (auto& handle : wrapper->handles) callback(handle);
    return JS_UNDEFINED;
}

JSValue js_handle_play(JSContext* c, JSValueConst t, int, JSValueConst*) { return withAnimationHandles(c, t, [](auto& h) { h.play(); }); }
JSValue js_handle_pause(JSContext* c, JSValueConst t, int, JSValueConst*) { return withAnimationHandles(c, t, [](auto& h) { h.pause(); }); }
JSValue js_handle_reverse(JSContext* c, JSValueConst t, int, JSValueConst*) { return withAnimationHandles(c, t, [](auto& h) { h.reverse(); }); }
JSValue js_handle_cancel(JSContext* c, JSValueConst t, int, JSValueConst*) { return withAnimationHandles(c, t, [](auto& h) { h.cancel(); }); }
JSValue js_handle_finish(JSContext* c, JSValueConst t, int, JSValueConst*) { return withAnimationHandles(c, t, [](auto& h) { h.finish(); }); }
JSValue js_handle_commit(JSContext* c, JSValueConst t, int, JSValueConst*) { return withAnimationHandles(c, t, [](auto& h) { h.commitFinalStyles(); }); }

JSValue js_handle_seek(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    double progress = 0.0;
    if (argc < 1 || JS_ToFloat64(ctx, &progress, argv[0]) < 0) return JS_ThrowTypeError(ctx, "progress is required");
    return withAnimationHandles(ctx, thisVal, [progress](auto& h) { h.seek(static_cast<float>(progress)); });
}

JSValue js_handle_progress(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* wrapper = static_cast<JsAnimationHandleWrapper*>(JS_GetOpaque2(ctx, thisVal, g_animation_handle_class_id));
    if (!wrapper || wrapper->handles.empty()) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, wrapper->handles.front().progress());
}

JSValue js_handle_setOnComplete(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* wrapper = static_cast<JsAnimationHandleWrapper*>(JS_GetOpaque2(ctx, thisVal, g_animation_handle_class_id));
    if (!wrapper) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "completion must be a function");
    if (!JS_IsUndefined(wrapper->completion)) JS_FreeValue(ctx, wrapper->completion);
    wrapper->completion = JS_DupValue(ctx, argv[0]);
    wrapper->completedCount = 0;
    wrapper->completionCalled = false;
    const size_t count = wrapper->handles.size();
    for (auto& handle : wrapper->handles) {
        handle.setOnComplete([wrapper, count] {
            ++wrapper->completedCount;
            if (wrapper->completionCalled || wrapper->completedCount < count) return;
            wrapper->completionCalled = true;
            JSValue result = JS_Call(wrapper->ctx, wrapper->completion, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(result)) JsRuntime::printException(wrapper->ctx);
            JS_FreeValue(wrapper->ctx, result);
        });
    }
    return JS_UNDEFINED;
}

// ------------------------------------------------------------
// Legacy low-level AnimationEngine adapter
// ------------------------------------------------------------
JSValue js_animation_engine_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target;
    (void)argc;
    (void)argv;

    JSValue obj = JS_NewObjectClass(ctx, g_animation_engine_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = new JsAnimationEngineWrapper();
    wrapper->engine = new lcl::ui::AnimationEngine();
    JS_SetOpaque(obj, wrapper);
    return obj;
}

JSValue js_animation_createChannel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t objectId = 0;
    int32_t propertyId = 0;
    double initialValue = 0.0;
    if (argc >= 1) JS_ToInt64(ctx, &objectId, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &propertyId, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &initialValue, argv[2]);

    lcl::ui::ChannelKey key{};
    key.objectId = static_cast<uint64_t>(std::max<int64_t>(0, objectId));
    key.propertyId = static_cast<uint32_t>(std::max<int32_t>(0, propertyId));
    lcl::ui::ChannelId id = wrap->engine->ensureChannel(key, static_cast<float>(initialValue));
    return JS_NewFloat64(ctx, static_cast<double>(id));
}

JSValue js_animation_animateSpring(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t channelId = 0;
    double target = 0.0;
    double omega = 18.0;
    double zeta = 1.0;
    double mass = 1.0;
    if (argc >= 1) JS_ToInt64(ctx, &channelId, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &target, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &omega, argv[2]);
    if (argc >= 4) JS_ToFloat64(ctx, &zeta, argv[3]);
    if (argc >= 5) JS_ToFloat64(ctx, &mass, argv[4]);

    lcl::ui::MotionSpec spec{};
    spec.mode = lcl::ui::MotionMode::Spring;
    spec.spring.mass = static_cast<float>(std::max(0.0001, mass));
    const float safeOmega = static_cast<float>(std::max(0.01, omega));
    const float safeZeta = static_cast<float>(std::max(0.01, zeta));
    spec.spring.omega = safeOmega;
    spec.spring.zeta = safeZeta;

    bool ok = wrap->engine->animateTo(static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channelId)),
                                      static_cast<float>(target),
                                      spec);
    return JS_NewBool(ctx, ok);
}

JSValue js_animation_animateTween(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t channelId = 0;
    double target = 0.0;
    double durationSec = 0.18;
    double delaySec = 0.0;
    std::string easing = "easeOutCubic";

    if (argc >= 1) JS_ToInt64(ctx, &channelId, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &target, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &durationSec, argv[2]);
    if (argc >= 4) {
        const char* str = JS_ToCString(ctx, argv[3]);
        if (str) {
            easing = str;
            JS_FreeCString(ctx, str);
        }
    }
    if (argc >= 5) JS_ToFloat64(ctx, &delaySec, argv[4]);

    lcl::ui::MotionSpec spec{};
    spec.mode = lcl::ui::MotionMode::Tween;
    spec.tween.durationSec = static_cast<float>(std::max(0.0001, durationSec));
    spec.tween.delaySec = static_cast<float>(std::max(0.0, delaySec));
    spec.tween.easing = easingFromString(easing);

    bool ok = wrap->engine->animateTo(static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channelId)),
                                      static_cast<float>(target),
                                      spec);
    return JS_NewBool(ctx, ok);
}

JSValue js_animation_retarget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t channelId = 0;
    double target = 0.0;
    if (argc >= 1) JS_ToInt64(ctx, &channelId, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &target, argv[1]);

    bool ok = wrap->engine->retarget(static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channelId)),
                                     static_cast<float>(target));
    return JS_NewBool(ctx, ok);
}

JSValue js_animation_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t channelId = 0;
    int snap = 1;
    if (argc >= 1) JS_ToInt64(ctx, &channelId, argv[0]);
    if (argc >= 2) snap = JS_ToBool(ctx, argv[1]);

    bool ok = wrap->engine->stop(static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channelId)), snap != 0);
    return JS_NewBool(ctx, ok);
}

JSValue js_animation_isActive(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t channelId = 0;
    if (argc >= 1) JS_ToInt64(ctx, &channelId, argv[0]);
    bool active = wrap->engine->isActive(static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channelId)));
    return JS_NewBool(ctx, active);
}

JSValue js_animation_sample(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t channelId = 0;
    if (argc >= 1) JS_ToInt64(ctx, &channelId, argv[0]);

    lcl::ui::AnimatedSample s = wrap->engine->sample(static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channelId)));
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "value", JS_NewFloat64(ctx, static_cast<double>(s.value)));
    JS_SetPropertyStr(ctx, out, "velocity", JS_NewFloat64(ctx, static_cast<double>(s.velocity)));
    JS_SetPropertyStr(ctx, out, "active", JS_NewBool(ctx, s.active));
    return out;
}

JSValue js_animation_tick(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    double dtSec = 1.0 / 60.0;
    if (argc >= 1) JS_ToFloat64(ctx, &dtSec, argv[0]);
    std::vector<lcl::ui::ChannelId> completed = wrap->engine->tick(static_cast<float>(std::max(0.0, dtSec)));

    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < static_cast<uint32_t>(completed.size()); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, static_cast<double>(completed[i])));
    }
    return arr;
}

JSValue js_animation_clearObjectChannels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    int64_t objectId = 0;
    if (argc >= 1) JS_ToInt64(ctx, &objectId, argv[0]);
    size_t count = wrap->engine->clearObjectChannels(static_cast<uint64_t>(std::max<int64_t>(0, objectId)));
    return JS_NewInt32(ctx, static_cast<int32_t>(count));
}

JSValue js_animation_clearAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* wrap = static_cast<JsAnimationEngineWrapper*>(JS_GetOpaque2(ctx, this_val, g_animation_engine_class_id));
    if (!wrap || !wrap->engine) return JS_EXCEPTION;

    wrap->engine->clearAll();
    return JS_UNDEFINED;
}

// ------------------------------------------------------------
// Button / Text Specific Methods
// ------------------------------------------------------------
JSValue js_button_setLabel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* btn = dynamic_cast<lcl::ui::Button*>(wrap->widget);
    if (btn && argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) {
            btn->setLabel(str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

JSValue js_button_getLabel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* btn = dynamic_cast<lcl::ui::Button*>(wrap->widget);
    if (btn) {
        return JS_NewString(ctx, btn->getLabel().c_str());
    }
    return JS_NewString(ctx, "");
}

JSValue js_button_setOnClick(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* btn = dynamic_cast<lcl::ui::Button*>(wrap->widget);
    auto* blurSurface = dynamic_cast<lcl::ui::BackdropSurface*>(wrap->widget);
    if ((btn || blurSurface) && argc >= 1 && JS_IsFunction(ctx, argv[0])) {
        JSValue funcVal = JS_DupValue(ctx, argv[0]);
        JSRuntime* rt = JS_GetRuntime(ctx);
        JSContext* ctxRef = ctx;

        struct JsCallbackHolder {
            JSRuntime* rt{nullptr};
            JSContext* ctx{nullptr};
            JSValue func{JS_UNDEFINED};
            ~JsCallbackHolder() {
                if (rt && !JS_IsUndefined(func)) {
                    JS_FreeValueRT(rt, func);
                }
            }
        };

        auto holder = std::make_shared<JsCallbackHolder>();
        holder->rt = rt;
        holder->ctx = ctxRef;
        holder->func = funcVal;

        auto clickThunk = [holder]() {
            JSValue ret = JS_Call(holder->ctx, holder->func, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(ret)) {
                JsRuntime::printException(holder->ctx);
            }
            JS_FreeValue(holder->ctx, ret);
        };

        if (btn) btn->setOnClick(clickThunk);
        if (blurSurface) blurSurface->setOnClick(clickThunk);

        wrap->widget->setGcMarkCallback([holder](void* rtPtr, void* markFuncPtr) {
            auto* rt = reinterpret_cast<JSRuntime*>(rtPtr);
            auto* mark_func = reinterpret_cast<JS_MarkFunc*>(markFuncPtr);
            if (holder && !JS_IsUndefined(holder->func)) {
                JS_MarkValue(rt, holder->func, mark_func);
            }
        });
    }
    return JS_UNDEFINED;
}

JSValue js_text_setText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* txt = dynamic_cast<lcl::ui::Text*>(wrap->widget);
    if (txt && argc >= 1) {
        const char* str = JS_ToCString(ctx, argv[0]);
        if (str) {
            txt->setText(str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

JSValue js_text_getText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* txt = dynamic_cast<lcl::ui::Text*>(wrap->widget);
    if (txt) {
        return JS_NewString(ctx, txt->getText().c_str());
    }
    return JS_NewString(ctx, "");
}

JSValue js_text_setFontSize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    auto* txt = dynamic_cast<lcl::ui::Text*>(wrap->widget);
    if (txt && argc >= 1) {
        double val = 14.0;
        JS_ToFloat64(ctx, &val, argv[0]);
        txt->setFontSize(static_cast<float>(val));
    }
    return JS_UNDEFINED;
}

} // namespace

JsRuntime::JsRuntime() = default;

JsRuntime::~JsRuntime() {
    shutdown();
}

bool JsRuntime::initialize() {
    if (m_initialized) return true;

    m_rt = JS_NewRuntime();
    if (!m_rt) {
        std::cerr << "[LCL JS ERROR] Failed to create QuickJS runtime.\n";
        return false;
    }

    m_ctx = JS_NewContext(m_rt);
    if (!m_ctx) {
        std::cerr << "[LCL JS ERROR] Failed to create QuickJS context.\n";
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
        return false;
    }

    registerLclBindings();

    m_initialized = true;
    std::cout << "[LCL JS] QuickJS runtime initialized successfully.\n";
    return true;
}

void JsRuntime::registerLclBindings() {
    JSValue global = JS_GetGlobalObject(m_ctx);

    // 1. console.log
    JSValue consoleObj = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, consoleObj, "log", JS_NewCFunction(m_ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(m_ctx, global, "console", consoleObj);

    // 2. Register Classes
    JS_NewClassID(m_rt, &g_window_app_class_id);
    JS_NewClass(m_rt, g_window_app_class_id, &js_window_app_class);

    JS_NewClassID(m_rt, &g_widget_class_id);
    JS_NewClass(m_rt, g_widget_class_id, &js_widget_class);

    JS_NewClassID(m_rt, &g_animation_engine_class_id);
    JS_NewClass(m_rt, g_animation_engine_class_id, &js_animation_engine_class);

    JS_NewClassID(m_rt, &g_animation_handle_class_id);
    JS_NewClass(m_rt, g_animation_handle_class_id, &js_animation_handle_class);

    // Prototypes
    JSValue windowAppProto = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, windowAppProto, "setRootWidget", JS_NewCFunction(m_ctx, js_window_app_setRootWidget, "setRootWidget", 1));
    JS_SetPropertyStr(m_ctx, windowAppProto, "connectCompositor", JS_NewCFunction(m_ctx, js_window_app_connectCompositor, "connectCompositor", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "runEventLoop", JS_NewCFunction(m_ctx, js_window_app_runEventLoop, "runEventLoop", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "tick", JS_NewCFunction(m_ctx, js_window_app_tick, "tick", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "renderFrame", JS_NewCFunction(m_ctx, js_window_app_renderFrame, "renderFrame", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "setSurfaceId", JS_NewCFunction(m_ctx, js_window_app_setSurfaceId, "setSurfaceId", 1));
    JS_SetPropertyStr(m_ctx, windowAppProto, "setAppId", JS_NewCFunction(m_ctx, js_window_app_setAppId, "setAppId", 1));
    JS_SetPropertyStr(m_ctx, windowAppProto, "sendPointerMove", JS_NewCFunction(m_ctx, js_window_app_sendPointerMove, "sendPointerMove", 2));
    JS_SetPropertyStr(m_ctx, windowAppProto, "sendPointerDown", JS_NewCFunction(m_ctx, js_window_app_sendPointerDown, "sendPointerDown", 3));
    JS_SetPropertyStr(m_ctx, windowAppProto, "sendPointerUp", JS_NewCFunction(m_ctx, js_window_app_sendPointerUp, "sendPointerUp", 3));
    JS_SetPropertyStr(m_ctx, windowAppProto, "requestWindowDrag", JS_NewCFunction(m_ctx, js_window_app_requestWindowDrag, "requestWindowDrag", 2));
    JS_SetPropertyStr(m_ctx, windowAppProto, "requestWindowMinimize", JS_NewCFunction(m_ctx, js_window_app_requestWindowMinimize, "requestWindowMinimize", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "requestWindowMaximize", JS_NewCFunction(m_ctx, js_window_app_requestWindowMaximize, "requestWindowMaximize", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "requestWindowRestore", JS_NewCFunction(m_ctx, js_window_app_requestWindowRestore, "requestWindowRestore", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "requestWindowToggleMaximize", JS_NewCFunction(m_ctx, js_window_app_requestWindowToggleMaximize, "requestWindowToggleMaximize", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "requestWindowClose", JS_NewCFunction(m_ctx, js_window_app_requestWindowClose, "requestWindowClose", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "setCsdTitlebarEnabled", JS_NewCFunction(m_ctx, js_window_app_setCsdTitlebarEnabled, "setCsdTitlebarEnabled", 1));
    JS_SetPropertyStr(m_ctx, windowAppProto, "configureCsdTitlebar", JS_NewCFunction(m_ctx, js_window_app_configureCsdTitlebar, "configureCsdTitlebar", 5));
    JS_SetPropertyStr(m_ctx, windowAppProto, "setDecorationMode", JS_NewCFunction(m_ctx, js_window_app_setDecorationMode, "setDecorationMode", 1));
    JS_SetPropertyStr(m_ctx, windowAppProto, "animate", JS_NewCFunction(m_ctx, js_window_app_animate, "animate", 2));
    JS_SetClassProto(m_ctx, g_window_app_class_id, windowAppProto);

    JSValue widgetProto = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, widgetProto, "setWidth", JS_NewCFunction(m_ctx, js_widget_setWidth, "setWidth", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setHeight", JS_NewCFunction(m_ctx, js_widget_setHeight, "setHeight", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setDirection", JS_NewCFunction(m_ctx, js_widget_setDirection, "setDirection", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setJustifyContent", JS_NewCFunction(m_ctx, js_widget_setJustifyContent, "setJustifyContent", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setAlignItems", JS_NewCFunction(m_ctx, js_widget_setAlignItems, "setAlignItems", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setPadding", JS_NewCFunction(m_ctx, js_widget_setPadding, "setPadding", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setGap", JS_NewCFunction(m_ctx, js_widget_setGap, "setGap", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "addChild", JS_NewCFunction(m_ctx, js_widget_addChild, "addChild", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setBackgroundColor", JS_NewCFunction(m_ctx, js_widget_setBackgroundColor, "setBackgroundColor", 4));
    JS_SetPropertyStr(m_ctx, widgetProto, "setBorderColor", JS_NewCFunction(m_ctx, js_widget_setBorderColor, "setBorderColor", 4));
    JS_SetPropertyStr(m_ctx, widgetProto, "setBorderWidth", JS_NewCFunction(m_ctx, js_widget_setBorderWidth, "setBorderWidth", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setBorderRadius", JS_NewCFunction(m_ctx, js_widget_setBorderRadius, "setBorderRadius", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "setBorderRoundness", JS_NewCFunction(m_ctx, js_widget_setBorderRoundness, "setBorderRoundness", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setLabel", JS_NewCFunction(m_ctx, js_button_setLabel, "setLabel", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "getLabel", JS_NewCFunction(m_ctx, js_button_getLabel, "getLabel", 0));
    JS_SetPropertyStr(m_ctx, widgetProto, "setOnClick", JS_NewCFunction(m_ctx, js_button_setOnClick, "setOnClick", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "addFilter", JS_NewCFunction(m_ctx, js_effect_addFilter, "addFilter", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "setGlass", JS_NewCFunction(m_ctx, js_effect_setGlass, "setGlass", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "clearFilters", JS_NewCFunction(m_ctx, js_effect_clearFilters, "clearFilters", 0));
    JS_SetPropertyStr(m_ctx, widgetProto, "setInteractive", JS_NewCFunction(m_ctx, js_backdrop_setInteractive, "setInteractive", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setOpacity", JS_NewCFunction(m_ctx, js_effect_setOpacity, "setOpacity", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setTranslation", JS_NewCFunction(m_ctx, js_widget_setTranslation, "setTranslation", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "setScale", JS_NewCFunction(m_ctx, js_widget_setScale, "setScale", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "setRotation", JS_NewCFunction(m_ctx, js_widget_setRotation, "setRotation", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setTransformOrigin", JS_NewCFunction(m_ctx, js_widget_setTransformOrigin, "setTransformOrigin", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "animate", JS_NewCFunction(m_ctx, js_widget_animate, "animate", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "setText", JS_NewCFunction(m_ctx, js_text_setText, "setText", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "getText", JS_NewCFunction(m_ctx, js_text_getText, "getText", 0));
    JS_SetPropertyStr(m_ctx, widgetProto, "setFontSize", JS_NewCFunction(m_ctx, js_text_setFontSize, "setFontSize", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setTextColor", JS_NewCFunction(m_ctx, js_text_setTextColor, "setTextColor", 4));
    JS_SetPropertyStr(m_ctx, widgetProto, "setSourcePath", JS_NewCFunction(m_ctx, js_image_setSourcePath, "setSourcePath", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "clearSource", JS_NewCFunction(m_ctx, js_image_clearSource, "clearSource", 0));
    JS_SetPropertyStr(m_ctx, widgetProto, "setFit", JS_NewCFunction(m_ctx, js_image_setFit, "setFit", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setCornerRadius", JS_NewCFunction(m_ctx, js_image_setCornerRadius, "setCornerRadius", 2));
    JS_SetPropertyStr(m_ctx, widgetProto, "setCornerRoundness", JS_NewCFunction(m_ctx, js_image_setCornerRoundness, "setCornerRoundness", 1));
    JS_SetClassProto(m_ctx, g_widget_class_id, widgetProto);

    JSValue animationProto = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, animationProto, "createChannel", JS_NewCFunction(m_ctx, js_animation_createChannel, "createChannel", 3));
    JS_SetPropertyStr(m_ctx, animationProto, "animateSpring", JS_NewCFunction(m_ctx, js_animation_animateSpring, "animateSpring", 5));
    JS_SetPropertyStr(m_ctx, animationProto, "animateTween", JS_NewCFunction(m_ctx, js_animation_animateTween, "animateTween", 5));
    JS_SetPropertyStr(m_ctx, animationProto, "retarget", JS_NewCFunction(m_ctx, js_animation_retarget, "retarget", 2));
    JS_SetPropertyStr(m_ctx, animationProto, "stop", JS_NewCFunction(m_ctx, js_animation_stop, "stop", 2));
    JS_SetPropertyStr(m_ctx, animationProto, "isActive", JS_NewCFunction(m_ctx, js_animation_isActive, "isActive", 1));
    JS_SetPropertyStr(m_ctx, animationProto, "sample", JS_NewCFunction(m_ctx, js_animation_sample, "sample", 1));
    JS_SetPropertyStr(m_ctx, animationProto, "tick", JS_NewCFunction(m_ctx, js_animation_tick, "tick", 1));
    JS_SetPropertyStr(m_ctx, animationProto, "clearObjectChannels", JS_NewCFunction(m_ctx, js_animation_clearObjectChannels, "clearObjectChannels", 1));
    JS_SetPropertyStr(m_ctx, animationProto, "clearAll", JS_NewCFunction(m_ctx, js_animation_clearAll, "clearAll", 0));
    JS_SetClassProto(m_ctx, g_animation_engine_class_id, animationProto);

    JSValue handleProto = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, handleProto, "play", JS_NewCFunction(m_ctx, js_handle_play, "play", 0));
    JS_SetPropertyStr(m_ctx, handleProto, "pause", JS_NewCFunction(m_ctx, js_handle_pause, "pause", 0));
    JS_SetPropertyStr(m_ctx, handleProto, "reverse", JS_NewCFunction(m_ctx, js_handle_reverse, "reverse", 0));
    JS_SetPropertyStr(m_ctx, handleProto, "cancel", JS_NewCFunction(m_ctx, js_handle_cancel, "cancel", 0));
    JS_SetPropertyStr(m_ctx, handleProto, "finish", JS_NewCFunction(m_ctx, js_handle_finish, "finish", 0));
    JS_SetPropertyStr(m_ctx, handleProto, "seek", JS_NewCFunction(m_ctx, js_handle_seek, "seek", 1));
    JS_SetPropertyStr(m_ctx, handleProto, "progress", JS_NewCFunction(m_ctx, js_handle_progress, "progress", 0));
    JS_SetPropertyStr(m_ctx, handleProto, "setOnComplete", JS_NewCFunction(m_ctx, js_handle_setOnComplete, "setOnComplete", 1));
    JS_SetPropertyStr(m_ctx, handleProto, "commitFinalStyles", JS_NewCFunction(m_ctx, js_handle_commit, "commitFinalStyles", 0));
    JS_SetClassProto(m_ctx, g_animation_handle_class_id, handleProto);

    // Global LCL namespace object
    JSValue lclObj = JS_NewObject(m_ctx);

    JSValue windowAppCtor = JS_NewCFunction2(m_ctx, js_window_app_constructor, "WindowApp", 3, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, windowAppCtor, windowAppProto);
    JS_SetPropertyStr(m_ctx, lclObj, "WindowApp", windowAppCtor);

    JSValue containerCtor = JS_NewCFunction2(m_ctx, js_container_constructor, "Container", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, containerCtor, widgetProto);
    JS_SetPropertyStr(m_ctx, lclObj, "Container", containerCtor);

    JSValue buttonCtor = JS_NewCFunction2(m_ctx, js_button_constructor, "Button", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, buttonCtor, widgetProto);
    JS_SetPropertyStr(m_ctx, lclObj, "Button", buttonCtor);

    JSValue backdropCtor = JS_NewCFunction2(m_ctx, js_backdrop_surface_constructor, "BackdropSurface", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, backdropCtor, widgetProto);
    JS_SetPropertyStr(m_ctx, lclObj, "BackdropSurface", backdropCtor);

    JSValue textCtor = JS_NewCFunction2(m_ctx, js_text_constructor, "Text", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, textCtor, widgetProto);
    JS_SetPropertyStr(m_ctx, lclObj, "Text", textCtor);

    JSValue imageCtor = JS_NewCFunction2(m_ctx, js_image_constructor, "Image", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, imageCtor, widgetProto);
    JS_SetPropertyStr(m_ctx, lclObj, "Image", imageCtor);

    JSValue animationCtor = JS_NewCFunction2(m_ctx, js_animation_engine_constructor, "AnimationEngine", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, animationCtor, animationProto);
    JS_SetPropertyStr(m_ctx, lclObj, "AnimationEngine", animationCtor);

    JS_SetPropertyStr(m_ctx, global, "LCL", lclObj);

    JS_FreeValue(m_ctx, global);
}

bool JsRuntime::evalFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "[LCL JS ERROR] Cannot open JS file: " << filePath << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return evalCode(buffer.str(), filePath);
}

bool JsRuntime::evalCode(const std::string& code, const std::string& filename) {
    if (!m_initialized && !initialize()) return false;

    JSValue val = JS_Eval(m_ctx, code.c_str(), code.size(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        printException(m_ctx);
        JS_FreeValue(m_ctx, val);
        return false;
    }

    JS_FreeValue(m_ctx, val);
    return true;
}

void JsRuntime::printException(JSContext* ctx) {
    JSValue exception_val = JS_GetException(ctx);
    const char* str = JS_ToCString(ctx, exception_val);
    if (str) {
        std::cerr << "[LCL JS Exception] " << str << std::endl;
        JS_FreeCString(ctx, str);
    }
    JSValue stack = JS_GetPropertyStr(ctx, exception_val, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* stack_str = JS_ToCString(ctx, stack);
        if (stack_str) {
            std::cerr << "[LCL JS Stack] " << stack_str << std::endl;
            JS_FreeCString(ctx, stack_str);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception_val);
}

void JsRuntime::shutdown() {
    if (!m_initialized) return;

    if (m_ctx) {
        JS_RunGC(m_rt);
        JS_FreeContext(m_ctx);
        m_ctx = nullptr;
    }
    if (m_rt) {
        JS_RunGC(m_rt);
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
    }
    m_initialized = false;
    std::cout << "[LCL JS] QuickJS runtime shutdown complete.\n";
}

} // namespace lcl::binding
