#include "system/javascript/js_binding_registry.hpp"
#include "system/javascript/js_binding_support.hpp"

#include <iostream>

namespace lcl::binding {
namespace {

JSValue consoleLog(JSContext* context, JSValueConst, int argc,
                   JSValueConst* argv) {
    for (int index = 0; index < argc; ++index) {
        const char* text = JS_ToCString(context, argv[index]);
        if (!text) return JS_EXCEPTION;
        if (index != 0) std::cout << ' ';
        std::cout << text;
        JS_FreeCString(context, text);
    }
    std::cout << std::endl;
    return JS_UNDEFINED;
}

} // namespace

void registerLclBindings(JSContext* context, JSRuntime* runtime) {
    JSValue global = JS_GetGlobalObject(context);
    JSValue console = JS_NewObject(context);
    JS_SetPropertyStr(context, console, "log",
                      JS_NewCFunction(context, consoleLog, "log", 1));
    JS_SetPropertyStr(context, global, "console", console);

    JSValue windowAppPrototype = JS_NewObject(context);
    JSValue widgetPrototype = JS_NewObject(context);
    JSValue lcl = JS_NewObject(context);

    registerWidgetExtensions(context, widgetPrototype);
    registerAnimationBindings(context, runtime, widgetPrototype, lcl);
    registerWindowAppBindings(context, windowAppPrototype, lcl);
    registerControlBindings(context, widgetPrototype, lcl);
    detail::registerCoreBindingClasses(context, runtime, windowAppPrototype,
                                       widgetPrototype);

    JS_SetPropertyStr(context, global, "LCL", lcl);
    JS_FreeValue(context, global);
}

} // namespace lcl::binding
