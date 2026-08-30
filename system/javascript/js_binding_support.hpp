#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "quickjs.h"

namespace lcl::ui {
class Widget;
class WindowApp;
}

namespace lcl::motion {
class Easing;
}

namespace lcl::binding::detail {

using CallbackStoreHandle = std::shared_ptr<void>;

lcl::ui::Widget* requireWidget(JSContext* context, JSValueConst value);
lcl::ui::WindowApp* requireWindowApp(JSContext* context, JSValueConst value);

JSValue wrapWidget(JSContext* context, JSValueConst newTarget,
                   lcl::ui::Widget* widget);
JSValue wrapWindowApp(JSContext* context, JSValueConst newTarget,
                      lcl::ui::WindowApp* window);
std::unique_ptr<lcl::ui::Widget> takeWidget(JSContext* context,
                                            JSValueConst value);

CallbackStoreHandle setWidgetCallback(JSContext* context, JSValueConst widget,
                                      const std::string& key,
                                      JSValueConst callback);
CallbackStoreHandle setWindowCallback(JSContext* context, JSValueConst window,
                                      const std::string& key,
                                      JSValueConst callback);
void invokeCallback(const CallbackStoreHandle& handle, const std::string& key,
                    int argc = 0, JSValueConst* argv = nullptr);
void invokeStringCallback(const CallbackStoreHandle& handle,
                          const std::string& key, const std::string& value);
void invokeBoolCallback(const CallbackStoreHandle& handle,
                        const std::string& key, bool value);
void invokeNumberCallback(const CallbackStoreHandle& handle,
                          const std::string& key, double value);
void invokeTwoNumberCallback(const CallbackStoreHandle& handle,
                             const std::string& key, double first,
                             double second);
bool invokePointerCallback(const CallbackStoreHandle& handle,
                           const std::string& key, const char* type,
                           float x, float y, int button,
                           uint32_t pointerId);

void addMethod(JSContext* context, JSValue prototype, const char* name,
               JSCFunction* function, int argumentCount);
JSValue makeDerivedPrototype(JSContext* context, JSValue basePrototype);
void addConstructor(JSContext* context, JSValue lclNamespace,
                    const char* name, JSCFunction* constructor,
                    int argumentCount, JSValue prototype);

void registerCoreBindingClasses(JSContext* context, JSRuntime* runtime,
                                JSValue windowAppPrototype,
                                JSValue widgetPrototype);

} // namespace lcl::binding::detail

namespace lcl::binding {

/** Parses the JavaScript easing grammar shared by window and widget motion. */
lcl::motion::Easing parseAnimationEasing(std::string_view value);

void registerWidgetExtensions(JSContext* context, JSValue widgetPrototype);
void registerControlBindings(JSContext* context, JSValue widgetPrototype,
                             JSValue lclNamespace);
void registerWindowAppBindings(JSContext* context, JSValue windowAppPrototype,
                               JSValue lclNamespace);
void registerAnimationBindings(JSContext* context, JSRuntime* runtime,
                               JSValue widgetPrototype,
                               JSValue lclNamespace);

} // namespace lcl::binding
