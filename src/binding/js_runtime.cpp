#include "binding/js_runtime.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/text.hpp"

namespace lcl::binding {

namespace {

// QuickJS Class IDs
JSClassID g_window_app_class_id = 0;
JSClassID g_widget_class_id = 0;

struct JsWindowAppWrapper {
    lcl::ui::WindowApp* app{nullptr};
};

struct JsWidgetWrapper {
    lcl::ui::Widget* widget{nullptr};
    bool ownedByCpp{false};
};

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
        if (!wrapper->ownedByCpp && wrapper->widget) {
            delete wrapper->widget;
            wrapper->widget = nullptr;
        }
        delete wrapper;
    }
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
    wrapper->app = new lcl::ui::WindowApp(width, height, title);
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

// ------------------------------------------------------------
// Widget / Container / Button / Text Constructors
// ------------------------------------------------------------
JSValue js_container_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    (void)new_target; (void)argc; (void)argv;
    JSValue obj = JS_NewObjectClass(ctx, g_widget_class_id);
    if (JS_IsException(obj)) return obj;

    auto* wrapper = new JsWidgetWrapper();
    wrapper->widget = new lcl::ui::Container();
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

    auto* wrapper = new JsWidgetWrapper();
    wrapper->widget = new lcl::ui::Button(label);
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

    auto* wrapper = new JsWidgetWrapper();
    wrapper->widget = new lcl::ui::Text(text);
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
        wrap->widget->getYogaNode().setWidth(static_cast<float>(val));
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setHeight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        double val = 0.0;
        JS_ToFloat64(ctx, &val, argv[0]);
        wrap->widget->getYogaNode().setHeight(static_cast<float>(val));
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
        wrap->widget->getYogaNode().setPadding(YGEdgeAll, static_cast<float>(val));
    }
    return JS_UNDEFINED;
}

JSValue js_widget_setGap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* wrap = static_cast<JsWidgetWrapper*>(JS_GetOpaque2(ctx, this_val, g_widget_class_id));
    if (!wrap || !wrap->widget) return JS_EXCEPTION;

    if (argc >= 1) {
        double val = 0.0;
        JS_ToFloat64(ctx, &val, argv[0]);
        wrap->widget->getYogaNode().setGap(YGGutterAll, static_cast<float>(val));
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
    if (btn && argc >= 1 && JS_IsFunction(ctx, argv[0])) {
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

        btn->setOnClick([holder]() {
            JSValue ret = JS_Call(holder->ctx, holder->func, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(ret)) {
                JsRuntime::printException(holder->ctx);
            }
            JS_FreeValue(holder->ctx, ret);
        });

        btn->setGcMarkCallback([holder](void* rtPtr, void* markFuncPtr) {
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

    // Prototypes
    JSValue windowAppProto = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, windowAppProto, "setRootWidget", JS_NewCFunction(m_ctx, js_window_app_setRootWidget, "setRootWidget", 1));
    JS_SetPropertyStr(m_ctx, windowAppProto, "connectCompositor", JS_NewCFunction(m_ctx, js_window_app_connectCompositor, "connectCompositor", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "runEventLoop", JS_NewCFunction(m_ctx, js_window_app_runEventLoop, "runEventLoop", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "renderFrame", JS_NewCFunction(m_ctx, js_window_app_renderFrame, "renderFrame", 0));
    JS_SetPropertyStr(m_ctx, windowAppProto, "sendPointerMove", JS_NewCFunction(m_ctx, js_window_app_sendPointerMove, "sendPointerMove", 2));
    JS_SetPropertyStr(m_ctx, windowAppProto, "sendPointerDown", JS_NewCFunction(m_ctx, js_window_app_sendPointerDown, "sendPointerDown", 3));
    JS_SetPropertyStr(m_ctx, windowAppProto, "sendPointerUp", JS_NewCFunction(m_ctx, js_window_app_sendPointerUp, "sendPointerUp", 3));
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
    JS_SetPropertyStr(m_ctx, widgetProto, "setLabel", JS_NewCFunction(m_ctx, js_button_setLabel, "setLabel", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "getLabel", JS_NewCFunction(m_ctx, js_button_getLabel, "getLabel", 0));
    JS_SetPropertyStr(m_ctx, widgetProto, "setOnClick", JS_NewCFunction(m_ctx, js_button_setOnClick, "setOnClick", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "setText", JS_NewCFunction(m_ctx, js_text_setText, "setText", 1));
    JS_SetPropertyStr(m_ctx, widgetProto, "getText", JS_NewCFunction(m_ctx, js_text_getText, "getText", 0));
    JS_SetPropertyStr(m_ctx, widgetProto, "setFontSize", JS_NewCFunction(m_ctx, js_text_setFontSize, "setFontSize", 1));
    JS_SetClassProto(m_ctx, g_widget_class_id, widgetProto);

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

    JSValue textCtor = JS_NewCFunction2(m_ctx, js_text_constructor, "Text", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(m_ctx, textCtor, widgetProto);
    JS_SetPropertyStr(m_ctx, lclObj, "Text", textCtor);

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
