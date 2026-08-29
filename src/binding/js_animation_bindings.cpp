#include "binding/js_binding_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "binding/js_runtime.hpp"
#include "lcl-ui/core/animation.hpp"
#include "lcl-ui/core/motion.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::binding {
namespace {

JSClassID g_animationEngineClassId = 0;
JSClassID g_animationHandleClassId = 0;

struct AnimationEngineWrapper {
    std::unique_ptr<lcl::ui::AnimationEngine> engine;
};

struct AnimationHandleWrapper {
    JSContext* context{nullptr};
    std::vector<lcl::motion::AnimationHandle> handles;
    JSValue completion{JS_UNDEFINED};
    size_t completedCount{0};
    bool completionCalled{false};
};

void animationEngineFinalizer(JSRuntime*, JSValue value) {
    delete static_cast<AnimationEngineWrapper*>(
        JS_GetOpaque(value, g_animationEngineClassId));
}

void animationHandleFinalizer(JSRuntime* runtime, JSValue value) {
    auto* wrapper = static_cast<AnimationHandleWrapper*>(
        JS_GetOpaque(value, g_animationHandleClassId));
    if (!wrapper) return;
    JS_FreeValueRT(runtime, wrapper->completion);
    delete wrapper;
}

void animationHandleGcMark(JSRuntime* runtime, JSValue value,
                           JS_MarkFunc* markFunc) {
    auto* wrapper = static_cast<AnimationHandleWrapper*>(
        JS_GetOpaque(value, g_animationHandleClassId));
    if (wrapper) JS_MarkValue(runtime, wrapper->completion, markFunc);
}

JSClassDef g_animationEngineClass{
    "AnimationEngine", animationEngineFinalizer, nullptr, nullptr, nullptr,
};
JSClassDef g_animationHandleClass{
    "AnimationHandle", animationHandleFinalizer, animationHandleGcMark,
    nullptr, nullptr,
};

bool numberValue(JSContext* context, JSValueConst value, double& output) {
    return JS_ToFloat64(context, &output, value) == 0 && std::isfinite(output);
}

std::string stringProperty(JSContext* context, JSValueConst object,
                           const char* name, const char* fallback) {
    JSValue value = JS_GetPropertyStr(context, object, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(context, value);
        return fallback;
    }
    const char* text = JS_ToCString(context, value);
    const std::string result = text ? text : fallback;
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, value);
    return result;
}

double numberProperty(JSContext* context, JSValueConst object, const char* name,
                      double fallback) {
    JSValue value = JS_GetPropertyStr(context, object, name);
    double result = fallback;
    if (!JS_IsUndefined(value) && !JS_IsNull(value)) numberValue(context, value, result);
    JS_FreeValue(context, value);
    return result;
}

lcl::motion::EasingName easingName(std::string_view name) {
    using Name = lcl::motion::EasingName;
    if (name == "linear") return Name::Linear;
    if (name == "easeInSine") return Name::EaseInSine;
    if (name == "easeOutSine") return Name::EaseOutSine;
    if (name == "easeInOutSine") return Name::EaseInOutSine;
    if (name == "easeInQuad") return Name::EaseInQuad;
    if (name == "easeOutQuad") return Name::EaseOutQuad;
    if (name == "easeInOutQuad") return Name::EaseInOutQuad;
    if (name == "easeInCubic") return Name::EaseInCubic;
    if (name == "easeOutCubic") return Name::EaseOutCubic;
    if (name == "easeInOutCubic") return Name::EaseInOutCubic;
    if (name == "easeInQuart") return Name::EaseInQuart;
    if (name == "easeOutQuart") return Name::EaseOutQuart;
    if (name == "easeInOutQuart") return Name::EaseInOutQuart;
    if (name == "easeInQuint") return Name::EaseInQuint;
    if (name == "easeOutQuint") return Name::EaseOutQuint;
    if (name == "easeInOutQuint") return Name::EaseInOutQuint;
    if (name == "easeInExpo") return Name::EaseInExpo;
    if (name == "easeOutExpo") return Name::EaseOutExpo;
    if (name == "easeInOutExpo") return Name::EaseInOutExpo;
    if (name == "easeInCirc") return Name::EaseInCirc;
    if (name == "easeOutCirc") return Name::EaseOutCirc;
    if (name == "easeInOutCirc") return Name::EaseInOutCirc;
    if (name == "easeInBack") return Name::EaseInBack;
    if (name == "easeOutBack") return Name::EaseOutBack;
    if (name == "easeInOutBack") return Name::EaseInOutBack;
    if (name == "easeInElastic") return Name::EaseInElastic;
    if (name == "easeOutElastic") return Name::EaseOutElastic;
    if (name == "easeInOutElastic") return Name::EaseInOutElastic;
    if (name == "easeInBounce") return Name::EaseInBounce;
    if (name == "easeOutBounce") return Name::EaseOutBounce;
    if (name == "easeInOutBounce") return Name::EaseInOutBounce;
    return Name::EaseOutCubic;
}

lcl::motion::Easing parseEasing(std::string_view name) {
    float x1 = 0.0f, y1 = 0.0f, x2 = 1.0f, y2 = 1.0f;
    if (std::sscanf(std::string(name).c_str(), "cubic-bezier(%f,%f,%f,%f)",
                    &x1, &y1, &x2, &y2) == 4) {
        return lcl::motion::Easing::cubicBezier(x1, y1, x2, y2);
    }
    unsigned steps = 0;
    char position[16]{};
    if (std::sscanf(std::string(name).c_str(), "steps(%u,%15[^)])", &steps,
                    position) == 2) {
        return lcl::motion::Easing::steps(
            steps, std::string_view(position) == "start"
                       ? lcl::motion::StepPosition::Start
                       : lcl::motion::StepPosition::End);
    }
    return lcl::motion::Easing(easingName(name));
}

std::optional<lcl::ui::AnimatableProperty> animatableProperty(
    std::string_view name) {
    using Property = lcl::ui::AnimatableProperty;
    if (name == "opacity") return Property::Opacity;
    if (name == "translationX" || name == "translateX") return Property::TranslationX;
    if (name == "translationY" || name == "translateY") return Property::TranslationY;
    if (name == "scaleX") return Property::ScaleX;
    if (name == "scaleY") return Property::ScaleY;
    if (name == "rotation") return Property::Rotation;
    if (name == "width") return Property::Width;
    if (name == "height") return Property::Height;
    if (name == "borderWidth") return Property::BorderWidth;
    if (name == "borderRadius") return Property::BorderRadius;
    return std::nullopt;
}

JSValue widgetAnimate(JSContext* context, JSValueConst self, int argc,
                      JSValueConst* argv) {
    auto* widget = detail::requireWidget(context, self);
    if (!widget) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(context, argv[0])) {
        return JS_ThrowTypeError(context, "animate requires a keyframe array");
    }
    if (!widget->getMotionCoordinator()) {
        return JS_ThrowTypeError(context,
            "widget must be mounted in a WindowApp before animate()");
    }

    JSValue length = JS_GetPropertyStr(context, argv[0], "length");
    uint32_t frameCount = 0;
    const bool validCount = JS_ToUint32(context, &frameCount, length) == 0 &&
                            frameCount != 0;
    JS_FreeValue(context, length);
    if (!validCount) return JS_ThrowRangeError(context, "animate requires at least one keyframe");

    lcl::motion::AnimationOptions options;
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        double duration = 180.0;
        if (!numberValue(context, argv[1], duration)) return JS_EXCEPTION;
        options.durationSec = static_cast<float>(std::max(0.0, duration) / 1000.0);
    } else if (argc >= 2 && JS_IsObject(argv[1])) {
        options.durationSec = static_cast<float>(std::max(0.0,
            numberProperty(context, argv[1], "duration", 180.0)) / 1000.0);
        options.delaySec = static_cast<float>(std::max(0.0,
            numberProperty(context, argv[1], "delay", 0.0)) / 1000.0);
        options.repeat = static_cast<uint32_t>(std::clamp(
            numberProperty(context, argv[1], "iterations", 1.0), 1.0, 10000.0) - 1.0);
        options.easing = parseEasing(stringProperty(context, argv[1], "easing", "linear"));
        options.fill = stringProperty(context, argv[1], "fill", "none") == "forwards"
            ? lcl::motion::FillMode::Forwards : lcl::motion::FillMode::None;
        const auto direction = stringProperty(context, argv[1], "direction", "normal");
        if (direction == "reverse") options.direction = lcl::motion::PlaybackDirection::Reverse;
        else if (direction == "alternate") options.direction = lcl::motion::PlaybackDirection::Alternate;
        else if (direction == "alternate-reverse") options.direction = lcl::motion::PlaybackDirection::AlternateReverse;
    }

    auto wrapper = std::make_unique<AnimationHandleWrapper>();
    wrapper->context = context;
    constexpr std::array<std::string_view, 11> properties{
        "opacity", "translationX", "translationY", "scaleX", "scaleY", "rotation",
        "width", "height", "borderWidth", "borderRadius", "scale"};
    for (const auto name : properties) {
        std::vector<lcl::motion::Keyframe> frames;
        for (uint32_t index = 0; index < frameCount; ++index) {
            JSValue frame = JS_GetPropertyUint32(context, argv[0], index);
            JSValue value = JS_GetPropertyStr(context, frame, name.data());
            if (!JS_IsUndefined(value)) {
                double number = 0.0;
                if (!numberValue(context, value, number)) {
                    JS_FreeValue(context, value);
                    JS_FreeValue(context, frame);
                    return JS_ThrowTypeError(context, "keyframe values must be finite numbers");
                }
                const float defaultOffset = frameCount == 1 ? 1.0f
                    : static_cast<float>(index) / static_cast<float>(frameCount - 1);
                frames.push_back({static_cast<float>(numberProperty(
                    context, frame, "offset", defaultOffset)), static_cast<float>(number)});
            }
            JS_FreeValue(context, value);
            JS_FreeValue(context, frame);
        }
        if (frames.empty()) continue;
        if (name == "scale") {
            wrapper->handles.push_back(widget->animate(
                lcl::ui::AnimatableProperty::ScaleX, frames, options));
            wrapper->handles.push_back(widget->animate(
                lcl::ui::AnimatableProperty::ScaleY, std::move(frames), options));
        } else if (const auto property = animatableProperty(name)) {
            wrapper->handles.push_back(widget->animate(*property, std::move(frames), options));
        }
    }
    if (wrapper->handles.empty()) {
        return JS_ThrowTypeError(context,
            "keyframes do not contain an animatable property");
    }

    JSValue object = JS_NewObjectClass(context, g_animationHandleClassId);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, wrapper.release());
    return object;
}

template <typename Operation>
JSValue applyToHandles(JSContext* context, JSValueConst self,
                       Operation&& operation) {
    auto* wrapper = static_cast<AnimationHandleWrapper*>(
        JS_GetOpaque2(context, self, g_animationHandleClassId));
    if (!wrapper) return JS_EXCEPTION;
    for (auto& handle : wrapper->handles) operation(handle);
    return JS_UNDEFINED;
}

JSValue handlePlay(JSContext* c, JSValueConst s, int, JSValueConst*) {
    return applyToHandles(c, s, [](auto& handle) { handle.play(); });
}
JSValue handlePause(JSContext* c, JSValueConst s, int, JSValueConst*) {
    return applyToHandles(c, s, [](auto& handle) { handle.pause(); });
}
JSValue handleReverse(JSContext* c, JSValueConst s, int, JSValueConst*) {
    return applyToHandles(c, s, [](auto& handle) { handle.reverse(); });
}
JSValue handleCancel(JSContext* c, JSValueConst s, int, JSValueConst*) {
    return applyToHandles(c, s, [](auto& handle) { handle.cancel(); });
}
JSValue handleFinish(JSContext* c, JSValueConst s, int, JSValueConst*) {
    return applyToHandles(c, s, [](auto& handle) { handle.finish(); });
}
JSValue handleCommit(JSContext* c, JSValueConst s, int, JSValueConst*) {
    return applyToHandles(c, s, [](auto& handle) { handle.commitFinalStyles(); });
}
JSValue handleSeek(JSContext* context, JSValueConst self, int argc,
                   JSValueConst* argv) {
    double progress = 0.0;
    if (argc < 1 || !numberValue(context, argv[0], progress)) {
        return JS_ThrowTypeError(context, "progress must be a finite number");
    }
    return applyToHandles(context, self, [progress](auto& handle) {
        handle.seek(static_cast<float>(progress));
    });
}
JSValue handleProgress(JSContext* context, JSValueConst self, int,
                       JSValueConst*) {
    auto* wrapper = static_cast<AnimationHandleWrapper*>(
        JS_GetOpaque2(context, self, g_animationHandleClassId));
    if (!wrapper || wrapper->handles.empty()) return JS_EXCEPTION;
    return JS_NewFloat64(context, wrapper->handles.front().progress());
}
JSValue handleOnComplete(JSContext* context, JSValueConst self, int argc,
                         JSValueConst* argv) {
    auto* wrapper = static_cast<AnimationHandleWrapper*>(
        JS_GetOpaque2(context, self, g_animationHandleClassId));
    if (!wrapper) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(context, argv[0])) {
        return JS_ThrowTypeError(context, "completion must be a function");
    }
    JS_FreeValue(context, wrapper->completion);
    wrapper->completion = JS_DupValue(context, argv[0]);
    wrapper->completedCount = 0;
    wrapper->completionCalled = false;
    const size_t count = wrapper->handles.size();
    for (auto& handle : wrapper->handles) {
        handle.setOnComplete([wrapper, count] {
            if (++wrapper->completedCount < count || wrapper->completionCalled) return;
            wrapper->completionCalled = true;
            JSValue result = JS_Call(wrapper->context, wrapper->completion,
                                     JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(result)) JsRuntime::printException(wrapper->context);
            JS_FreeValue(wrapper->context, result);
        });
    }
    return JS_UNDEFINED;
}

AnimationEngineWrapper* animationEngine(JSContext* context, JSValueConst value) {
    return static_cast<AnimationEngineWrapper*>(
        JS_GetOpaque2(context, value, g_animationEngineClassId));
}
JSValue animationEngineConstructor(JSContext* context, JSValueConst, int,
                                   JSValueConst*) {
    JSValue object = JS_NewObjectClass(context, g_animationEngineClassId);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new AnimationEngineWrapper{std::make_unique<lcl::ui::AnimationEngine>()});
    return object;
}
JSValue engineCreateChannel(JSContext* context, JSValueConst self, int argc,
                            JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t objectId = 0;
    int32_t propertyId = 0;
    double initial = 0.0;
    if ((argc > 0 && JS_ToInt64(context, &objectId, argv[0]) < 0) ||
        (argc > 1 && JS_ToInt32(context, &propertyId, argv[1]) < 0) ||
        (argc > 2 && !numberValue(context, argv[2], initial))) {
        return JS_ThrowTypeError(context, "invalid channel arguments");
    }
    return JS_NewFloat64(context, wrapper->engine->ensureChannel(
        {static_cast<uint64_t>(std::max<int64_t>(0, objectId)),
         static_cast<uint32_t>(std::max(0, propertyId))}, static_cast<float>(initial)));
}
JSValue engineSpring(JSContext* context, JSValueConst self, int argc,
                     JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t channel = 0;
    double target = 0.0, omega = 18.0, zeta = 1.0, mass = 1.0;
    if ((argc > 0 && JS_ToInt64(context, &channel, argv[0]) < 0) ||
        (argc > 1 && !numberValue(context, argv[1], target)) ||
        (argc > 2 && !numberValue(context, argv[2], omega)) ||
        (argc > 3 && !numberValue(context, argv[3], zeta)) ||
        (argc > 4 && !numberValue(context, argv[4], mass))) {
        return JS_ThrowTypeError(context, "invalid spring arguments");
    }
    lcl::ui::MotionSpec spec;
    spec.mode = lcl::ui::MotionMode::Spring;
    spec.spring.mass = static_cast<float>(std::max(0.0001, mass));
    spec.spring.omega = static_cast<float>(std::max(0.01, omega));
    spec.spring.zeta = static_cast<float>(std::max(0.01, zeta));
    return JS_NewBool(context, wrapper->engine->animateTo(
        static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channel)),
        static_cast<float>(target), spec));
}
JSValue engineTween(JSContext* context, JSValueConst self, int argc,
                    JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t channel = 0;
    double target = 0.0, duration = 0.18, delay = 0.0;
    if ((argc > 0 && JS_ToInt64(context, &channel, argv[0]) < 0) ||
        (argc > 1 && !numberValue(context, argv[1], target)) ||
        (argc > 2 && !numberValue(context, argv[2], duration)) ||
        (argc > 4 && !numberValue(context, argv[4], delay))) {
        return JS_ThrowTypeError(context, "invalid tween arguments");
    }
    std::string easing = "easeOutCubic";
    if (argc > 3) {
        const char* text = JS_ToCString(context, argv[3]);
        if (!text) return JS_EXCEPTION;
        easing = text;
        JS_FreeCString(context, text);
    }
    lcl::ui::MotionSpec spec;
    spec.mode = lcl::ui::MotionMode::Tween;
    spec.tween.durationSec = static_cast<float>(std::max(0.0001, duration));
    spec.tween.delaySec = static_cast<float>(std::max(0.0, delay));
    spec.tween.easing = easingName(easing);
    return JS_NewBool(context, wrapper->engine->animateTo(
        static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channel)),
        static_cast<float>(target), spec));
}
JSValue engineRetarget(JSContext* context, JSValueConst self, int argc,
                       JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t channel = 0;
    double target = 0.0;
    if (argc < 2 || JS_ToInt64(context, &channel, argv[0]) < 0 ||
        !numberValue(context, argv[1], target)) {
        return JS_ThrowTypeError(context, "retarget requires channel and target");
    }
    return JS_NewBool(context, wrapper->engine->retarget(
        static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channel)),
        static_cast<float>(target)));
}
JSValue engineStop(JSContext* context, JSValueConst self, int argc,
                   JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t channel = 0;
    if (argc < 1 || JS_ToInt64(context, &channel, argv[0]) < 0) {
        return JS_ThrowTypeError(context, "channel is required");
    }
    const bool snap = argc < 2 || JS_ToBool(context, argv[1]) != 0;
    return JS_NewBool(context, wrapper->engine->stop(
        static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channel)), snap));
}
JSValue engineIsActive(JSContext* context, JSValueConst self, int argc,
                       JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t channel = 0;
    if (argc < 1 || JS_ToInt64(context, &channel, argv[0]) < 0) {
        return JS_ThrowTypeError(context, "channel is required");
    }
    return JS_NewBool(context, wrapper->engine->isActive(
        static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channel))));
}
JSValue engineSample(JSContext* context, JSValueConst self, int argc,
                     JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t channel = 0;
    if (argc < 1 || JS_ToInt64(context, &channel, argv[0]) < 0) {
        return JS_ThrowTypeError(context, "channel is required");
    }
    const auto sample = wrapper->engine->sample(
        static_cast<lcl::ui::ChannelId>(std::max<int64_t>(0, channel)));
    JSValue object = JS_NewObject(context);
    JS_SetPropertyStr(context, object, "value", JS_NewFloat64(context, sample.value));
    JS_SetPropertyStr(context, object, "velocity", JS_NewFloat64(context, sample.velocity));
    JS_SetPropertyStr(context, object, "active", JS_NewBool(context, sample.active));
    return object;
}
JSValue engineTick(JSContext* context, JSValueConst self, int argc,
                   JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    double dt = 1.0 / 60.0;
    if (argc > 0 && !numberValue(context, argv[0], dt)) {
        return JS_ThrowTypeError(context, "dt must be finite");
    }
    const auto completed = wrapper->engine->tick(static_cast<float>(std::max(0.0, dt)));
    JSValue result = JS_NewArray(context);
    for (uint32_t index = 0; index < completed.size(); ++index) {
        JS_SetPropertyUint32(context, result, index,
                             JS_NewFloat64(context, completed[index]));
    }
    return result;
}
JSValue engineClearObject(JSContext* context, JSValueConst self, int argc,
                          JSValueConst* argv) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    int64_t object = 0;
    if (argc < 1 || JS_ToInt64(context, &object, argv[0]) < 0) {
        return JS_ThrowTypeError(context, "object id is required");
    }
    return JS_NewInt64(context, static_cast<int64_t>(wrapper->engine->clearObjectChannels(
        static_cast<uint64_t>(std::max<int64_t>(0, object)))));
}
JSValue engineClearAll(JSContext* context, JSValueConst self, int,
                       JSValueConst*) {
    auto* wrapper = animationEngine(context, self);
    if (!wrapper) return JS_EXCEPTION;
    wrapper->engine->clearAll();
    return JS_UNDEFINED;
}

} // namespace

lcl::motion::Easing parseAnimationEasing(std::string_view value) {
    return parseEasing(value);
}

void registerAnimationBindings(JSContext* context, JSRuntime* runtime,
                               JSValue widgetPrototype, JSValue lclNamespace) {
    if (g_animationEngineClassId == 0) JS_NewClassID(runtime, &g_animationEngineClassId);
    if (g_animationHandleClassId == 0) JS_NewClassID(runtime, &g_animationHandleClassId);
    JS_NewClass(runtime, g_animationEngineClassId, &g_animationEngineClass);
    JS_NewClass(runtime, g_animationHandleClassId, &g_animationHandleClass);

    detail::addMethod(context, widgetPrototype, "animate", widgetAnimate, 2);

    JSValue handlePrototype = JS_NewObject(context);
    detail::addMethod(context, handlePrototype, "play", handlePlay, 0);
    detail::addMethod(context, handlePrototype, "pause", handlePause, 0);
    detail::addMethod(context, handlePrototype, "reverse", handleReverse, 0);
    detail::addMethod(context, handlePrototype, "cancel", handleCancel, 0);
    detail::addMethod(context, handlePrototype, "finish", handleFinish, 0);
    detail::addMethod(context, handlePrototype, "seek", handleSeek, 1);
    detail::addMethod(context, handlePrototype, "progress", handleProgress, 0);
    detail::addMethod(context, handlePrototype, "setOnComplete", handleOnComplete, 1);
    detail::addMethod(context, handlePrototype, "commitFinalStyles", handleCommit, 0);
    JS_SetClassProto(context, g_animationHandleClassId, handlePrototype);

    JSValue enginePrototype = JS_NewObject(context);
    detail::addMethod(context, enginePrototype, "createChannel", engineCreateChannel, 3);
    detail::addMethod(context, enginePrototype, "animateSpring", engineSpring, 5);
    detail::addMethod(context, enginePrototype, "animateTween", engineTween, 5);
    detail::addMethod(context, enginePrototype, "retarget", engineRetarget, 2);
    detail::addMethod(context, enginePrototype, "stop", engineStop, 2);
    detail::addMethod(context, enginePrototype, "isActive", engineIsActive, 1);
    detail::addMethod(context, enginePrototype, "sample", engineSample, 1);
    detail::addMethod(context, enginePrototype, "tick", engineTick, 1);
    detail::addMethod(context, enginePrototype, "clearObjectChannels", engineClearObject, 1);
    detail::addMethod(context, enginePrototype, "clearAll", engineClearAll, 0);
    detail::addConstructor(context, lclNamespace, "AnimationEngine",
                           animationEngineConstructor, 0, enginePrototype);
    JS_SetClassProto(context, g_animationEngineClassId, enginePrototype);
}

} // namespace lcl::binding
