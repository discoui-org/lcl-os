#include "binding/js_binding_support.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "binding/js_runtime.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::binding {
namespace {

JSClassID g_windowAppClassId = 0;
JSClassID g_widgetClassId = 0;

struct JsValueStore {
    JSRuntime* runtime{nullptr};
    JSContext* context{nullptr};
    std::unordered_map<std::string, JSValue> values;

    ~JsValueStore() {
        for (auto& [name, value] : values) {
            (void)name;
            JS_FreeValueRT(runtime, value);
        }
    }

    void set(std::string name, JSValueConst value) {
        auto found = values.find(name);
        if (found == values.end()) {
            values.emplace(std::move(name), JS_DupValue(context, value));
            return;
        }
        JS_FreeValueRT(runtime, found->second);
        found->second = JS_DupValue(context, value);
    }

    void erase(const std::string& name) {
        const auto found = values.find(name);
        if (found == values.end()) return;
        JS_FreeValueRT(runtime, found->second);
        values.erase(found);
    }

    JSValueConst get(const std::string& name) const {
        const auto found = values.find(name);
        return found == values.end() ? JS_UNDEFINED : found->second;
    }

    void mark(JSRuntime* runtime, JS_MarkFunc* markFunc) const {
        for (const auto& [name, value] : values) {
            (void)name;
            JS_MarkValue(runtime, value, markFunc);
        }
    }
};

std::shared_ptr<JsValueStore> makeValueStore(JSContext* context) {
    auto store = std::make_shared<JsValueStore>();
    store->runtime = JS_GetRuntime(context);
    store->context = context;
    return store;
}

struct JsWidgetWrapper {
    lcl::ui::Widget* widget{nullptr};
    std::shared_ptr<JsValueStore> callbacks;
    bool ownedByCpp{false};
};

struct JsWindowAppWrapper {
    lcl::ui::WindowApp* app{nullptr};
    std::weak_ptr<uint8_t> lifetime;
    std::shared_ptr<JsValueStore> callbacks;
    bool ownedByCpp{false};
};

void markWidgetTree(lcl::ui::Widget* widget, JSRuntime* runtime,
                    JS_MarkFunc* markFunc) {
    if (!widget) return;
    if (const auto& callback = widget->getGcMarkCallback()) {
        callback(static_cast<void*>(runtime), reinterpret_cast<void*>(markFunc));
    }
    for (const auto& child : widget->getChildren()) {
        markWidgetTree(child.get(), runtime, markFunc);
    }
}

void widgetFinalizer(JSRuntime*, JSValue value) {
    auto* wrapper = static_cast<JsWidgetWrapper*>(
        JS_GetOpaque(value, g_widgetClassId));
    if (!wrapper) return;
    if (wrapper->widget) wrapper->widget->setDestructionCallback(nullptr);
    if (!wrapper->ownedByCpp && wrapper->widget) delete wrapper->widget;
    delete wrapper;
}

void windowAppFinalizer(JSRuntime*, JSValue value) {
    auto* wrapper = static_cast<JsWindowAppWrapper*>(
        JS_GetOpaque(value, g_windowAppClassId));
    if (!wrapper) return;
    if (!wrapper->ownedByCpp && wrapper->app && !wrapper->lifetime.expired()) {
        delete wrapper->app;
    }
    delete wrapper;
}

void widgetGcMark(JSRuntime* runtime, JSValue value, JS_MarkFunc* markFunc) {
    auto* wrapper = static_cast<JsWidgetWrapper*>(
        JS_GetOpaque(value, g_widgetClassId));
    if (wrapper) wrapper->callbacks->mark(runtime, markFunc);
}

void windowAppGcMark(JSRuntime* runtime, JSValue value, JS_MarkFunc* markFunc) {
    auto* wrapper = static_cast<JsWindowAppWrapper*>(
        JS_GetOpaque(value, g_windowAppClassId));
    if (!wrapper) return;
    wrapper->callbacks->mark(runtime, markFunc);
    if (!wrapper->lifetime.expired() && wrapper->app) {
        markWidgetTree(wrapper->app->getRootWidget(), runtime, markFunc);
    }
}

JSClassDef g_widgetClass{
    "Widget", widgetFinalizer, widgetGcMark, nullptr, nullptr,
};
JSClassDef g_windowAppClass{
    "WindowApp", windowAppFinalizer, windowAppGcMark, nullptr, nullptr,
};

JsWidgetWrapper* checkedWidget(JSContext* context, JSValueConst value) {
    auto* wrapper = static_cast<JsWidgetWrapper*>(
        JS_GetOpaque2(context, value, g_widgetClassId));
    if (!wrapper || !wrapper->widget) {
        if (wrapper) JS_ThrowTypeError(context, "Widget is no longer alive");
        return nullptr;
    }
    return wrapper;
}

JsWindowAppWrapper* checkedWindowApp(JSContext* context, JSValueConst value) {
    auto* wrapper = static_cast<JsWindowAppWrapper*>(
        JS_GetOpaque2(context, value, g_windowAppClassId));
    if (!wrapper) return nullptr;
    if (wrapper->lifetime.expired()) wrapper->app = nullptr;
    if (!wrapper->app) {
        JS_ThrowTypeError(context, "WindowApp is no longer alive");
        return nullptr;
    }
    return wrapper;
}

std::shared_ptr<JsValueStore> asStore(
    const detail::CallbackStoreHandle& handle) {
    return std::static_pointer_cast<JsValueStore>(handle);
}

void invokeStoredCallback(const std::shared_ptr<JsValueStore>& store,
                          const std::string& key, int argc,
                          JSValueConst* argv) {
    if (!store) return;
    const JSValueConst callback = store->get(key);
    if (!JS_IsFunction(store->context, callback)) return;
    JSValue result = JS_Call(store->context, callback, JS_UNDEFINED, argc, argv);
    if (JS_IsException(result)) JsRuntime::printException(store->context);
    JS_FreeValue(store->context, result);
}

} // namespace

namespace detail {

lcl::ui::Widget* requireWidget(JSContext* context, JSValueConst value) {
    auto* wrapper = checkedWidget(context, value);
    return wrapper ? wrapper->widget : nullptr;
}

lcl::ui::WindowApp* requireWindowApp(JSContext* context, JSValueConst value) {
    auto* wrapper = checkedWindowApp(context, value);
    return wrapper ? wrapper->app : nullptr;
}

JSValue wrapWidget(JSContext* context, JSValueConst newTarget,
                   lcl::ui::Widget* widget) {
    JSValue object = JS_NewObjectClass(context, g_widgetClassId);
    if (JS_IsException(object)) {
        delete widget;
        return object;
    }
    JSValue prototype = JS_GetPropertyStr(context, newTarget, "prototype");
    if (JS_IsObject(prototype)) JS_SetPrototype(context, object, prototype);
    JS_FreeValue(context, prototype);

    auto* wrapper = new JsWidgetWrapper{widget, makeValueStore(context)};
    widget->setDestructionCallback([wrapper] { wrapper->widget = nullptr; });
    const auto callbacks = wrapper->callbacks;
    widget->setGcMarkCallback([callbacks](void* runtime, void* markFunc) {
        callbacks->mark(static_cast<JSRuntime*>(runtime),
                        reinterpret_cast<JS_MarkFunc*>(markFunc));
    });
    JS_SetOpaque(object, wrapper);
    return object;
}

JSValue wrapWindowApp(JSContext* context, JSValueConst newTarget,
                      lcl::ui::WindowApp* window) {
    JSValue object = JS_NewObjectClass(context, g_windowAppClassId);
    if (JS_IsException(object)) {
        delete window;
        return object;
    }
    JSValue prototype = JS_GetPropertyStr(context, newTarget, "prototype");
    if (JS_IsObject(prototype)) JS_SetPrototype(context, object, prototype);
    JS_FreeValue(context, prototype);
    auto* wrapper = new JsWindowAppWrapper{
        window, window->getLifetimeToken(), makeValueStore(context), false};
    JS_SetOpaque(object, wrapper);
    return object;
}

std::unique_ptr<lcl::ui::Widget> takeWidget(JSContext* context,
                                            JSValueConst value) {
    auto* wrapper = checkedWidget(context, value);
    if (!wrapper) return nullptr;
    if (wrapper->ownedByCpp) {
        JS_ThrowTypeError(context, "Widget ownership was already transferred");
        return nullptr;
    }
    wrapper->ownedByCpp = true;
    return std::unique_ptr<lcl::ui::Widget>(wrapper->widget);
}

CallbackStoreHandle setWidgetCallback(JSContext* context, JSValueConst widget,
                                      const std::string& key,
                                      JSValueConst callback) {
    auto* wrapper = checkedWidget(context, widget);
    if (!wrapper) return {};
    if (JS_IsNull(callback) || JS_IsUndefined(callback)) {
        wrapper->callbacks->erase(key);
        return wrapper->callbacks;
    }
    if (!JS_IsFunction(context, callback)) {
        JS_ThrowTypeError(context, "%s callback must be a function or null", key.c_str());
        return {};
    }
    wrapper->callbacks->set(key, callback);
    return wrapper->callbacks;
}

CallbackStoreHandle setWindowCallback(JSContext* context, JSValueConst window,
                                      const std::string& key,
                                      JSValueConst callback) {
    auto* wrapper = checkedWindowApp(context, window);
    if (!wrapper) return {};
    if (JS_IsNull(callback) || JS_IsUndefined(callback)) {
        wrapper->callbacks->erase(key);
        return wrapper->callbacks;
    }
    if (!JS_IsFunction(context, callback)) {
        JS_ThrowTypeError(context, "%s callback must be a function or null", key.c_str());
        return {};
    }
    wrapper->callbacks->set(key, callback);
    return wrapper->callbacks;
}

void invokeCallback(const CallbackStoreHandle& handle, const std::string& key,
                    int argc, JSValueConst* argv) {
    invokeStoredCallback(asStore(handle), key, argc, argv);
}

void invokeStringCallback(const CallbackStoreHandle& handle,
                          const std::string& key, const std::string& value) {
    const auto store = asStore(handle);
    if (!store) return;
    JSValue argument = JS_NewStringLen(store->context, value.data(), value.size());
    invokeStoredCallback(store, key, 1, &argument);
    JS_FreeValue(store->context, argument);
}

void invokeBoolCallback(const CallbackStoreHandle& handle, const std::string& key,
                        bool value) {
    const auto store = asStore(handle);
    if (!store) return;
    JSValue argument = JS_NewBool(store->context, value);
    invokeStoredCallback(store, key, 1, &argument);
    JS_FreeValue(store->context, argument);
}

void invokeNumberCallback(const CallbackStoreHandle& handle,
                          const std::string& key, double value) {
    const auto store = asStore(handle);
    if (!store) return;
    JSValue argument = JS_NewFloat64(store->context, value);
    invokeStoredCallback(store, key, 1, &argument);
    JS_FreeValue(store->context, argument);
}

void invokeTwoNumberCallback(const CallbackStoreHandle& handle,
                             const std::string& key, double first,
                             double second) {
    const auto store = asStore(handle);
    if (!store) return;
    std::array<JSValue, 2> arguments{
        JS_NewFloat64(store->context, first), JS_NewFloat64(store->context, second)};
    invokeStoredCallback(store, key, static_cast<int>(arguments.size()), arguments.data());
    for (JSValue argument : arguments) JS_FreeValue(store->context, argument);
}

void addMethod(JSContext* context, JSValue prototype, const char* name,
               JSCFunction* function, int argumentCount) {
    JS_SetPropertyStr(context, prototype, name,
                      JS_NewCFunction(context, function, name, argumentCount));
}

JSValue makeDerivedPrototype(JSContext* context, JSValue basePrototype) {
    JSValue prototype = JS_NewObject(context);
    JS_SetPrototype(context, prototype, basePrototype);
    return prototype;
}

void addConstructor(JSContext* context, JSValue lclNamespace, const char* name,
                    JSCFunction* constructor, int argumentCount,
                    JSValue prototype) {
    JSValue function = JS_NewCFunction2(context, constructor, name, argumentCount,
                                        JS_CFUNC_constructor, 0);
    JS_SetConstructor(context, function, prototype);
    JS_SetPropertyStr(context, lclNamespace, name, function);
}

void registerCoreBindingClasses(JSContext* context, JSRuntime* runtime,
                                JSValue windowAppPrototype,
                                JSValue widgetPrototype) {
    if (g_windowAppClassId == 0) JS_NewClassID(runtime, &g_windowAppClassId);
    if (g_widgetClassId == 0) JS_NewClassID(runtime, &g_widgetClassId);
    JS_NewClass(runtime, g_windowAppClassId, &g_windowAppClass);
    JS_NewClass(runtime, g_widgetClassId, &g_widgetClass);
    JS_SetClassProto(context, g_windowAppClassId, windowAppPrototype);
    JS_SetClassProto(context, g_widgetClassId, widgetPrototype);
}

} // namespace detail
} // namespace lcl::binding
