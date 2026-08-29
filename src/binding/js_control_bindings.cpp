#include "binding/js_binding_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/divider.hpp"
#include "lcl-ui/widgets/filter_group.hpp"
#include "lcl-ui/widgets/focus_scope.hpp"
#include "lcl-ui/widgets/image.hpp"
#include "lcl-ui/widgets/progress_view.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/slider.hpp"
#include "lcl-ui/widgets/tab_view.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/text_field.hpp"
#include "lcl-ui/widgets/toggle.hpp"

namespace lcl::binding {
namespace {

template <typename Type>
Type* control(JSContext* ctx, JSValueConst value, const char* name) {
    auto* base = detail::requireWidget(ctx, value);
    if (!base) return nullptr;
    auto* result = dynamic_cast<Type*>(base);
    if (!result) JS_ThrowTypeError(ctx, "%s method called on incompatible widget", name);
    return result;
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

bool colorValue(JSContext* ctx, int argc, JSValueConst* argv,
                lcl::graphics::Color& result) {
    std::array<int32_t, 4> channels{0, 0, 0, 255};
    if (argc == 1 && (JS_IsArray(ctx, argv[0]) || JS_IsObject(argv[0]))) {
        constexpr std::array<const char*, 4> names{"r", "g", "b", "a"};
        const bool array = JS_IsArray(ctx, argv[0]);
        for (uint32_t index = 0; index < channels.size(); ++index) {
            JSValue value = array ? JS_GetPropertyUint32(ctx, argv[0], index)
                                  : JS_GetPropertyStr(ctx, argv[0], names[index]);
            if (index < 3 && JS_IsUndefined(value)) {
                JS_FreeValue(ctx, value);
                return false;
            }
            const bool valid = JS_IsUndefined(value) ||
                JS_ToInt32(ctx, &channels[index], value) == 0;
            JS_FreeValue(ctx, value);
            if (!valid) return false;
        }
    } else {
        if (argc < 3) return false;
        for (int index = 0; index < argc && index < 4; ++index) {
            if (JS_ToInt32(ctx, &channels[index], argv[index]) < 0) return false;
        }
    }
    result = {static_cast<uint8_t>(std::clamp(channels[0], 0, 255)),
              static_cast<uint8_t>(std::clamp(channels[1], 0, 255)),
              static_cast<uint8_t>(std::clamp(channels[2], 0, 255)),
              static_cast<uint8_t>(std::clamp(channels[3], 0, 255))};
    return true;
}

template <typename Type, typename Setter>
JSValue setNumber(JSContext* ctx, JSValueConst self, int argc,
                  JSValueConst* argv, const char* name, Setter setter) {
    auto* target = control<Type>(ctx, self, name);
    if (!target) return JS_EXCEPTION;
    double value = 0.0;
    if (argc < 1 || !numberValue(ctx, argv[0], value))
        return JS_ThrowTypeError(ctx, "%s requires a finite number", name);
    setter(*target, static_cast<float>(value));
    return JS_UNDEFINED;
}

template <typename Type, typename Setter>
JSValue setBool(JSContext* ctx, JSValueConst self, int argc,
                JSValueConst* argv, const char* name, Setter setter) {
    auto* target = control<Type>(ctx, self, name);
    if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "%s requires a boolean", name);
    setter(*target, JS_ToBool(ctx, argv[0]) != 0);
    return JS_UNDEFINED;
}

JSValue containerCtor(JSContext* ctx, JSValueConst target, int, JSValueConst*) {
    return detail::wrapWidget(ctx, target, new lcl::ui::Container());
}
JSValue focusScopeCtor(JSContext* ctx, JSValueConst target, int, JSValueConst*) {
    return detail::wrapWidget(ctx, target, new lcl::ui::FocusScope());
}
JSValue dividerCtor(JSContext* ctx, JSValueConst target, int, JSValueConst*) {
    return detail::wrapWidget(ctx, target, new lcl::ui::Divider());
}
JSValue backdropCtor(JSContext* ctx, JSValueConst target, int, JSValueConst*) {
    return detail::wrapWidget(ctx, target, new lcl::ui::BackdropSurface());
}
JSValue filterGroupCtor(JSContext* ctx, JSValueConst target, int, JSValueConst*) {
    return detail::wrapWidget(ctx, target, new lcl::ui::FilterGroup());
}

JSValue buttonCtor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    const auto label = argc > 0 ? stringValue(ctx, argv[0]) : std::optional<std::string>{""};
    if (!label) return JS_EXCEPTION;
    return detail::wrapWidget(ctx, target, new lcl::ui::Button(*label));
}

JSValue textCtor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    const auto text = argc > 0 ? stringValue(ctx, argv[0]) : std::optional<std::string>{""};
    if (!text) return JS_EXCEPTION;
    return detail::wrapWidget(ctx, target, new lcl::ui::Text(*text));
}

JSValue textFieldCtor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    const auto text = argc > 0 ? stringValue(ctx, argv[0]) : std::optional<std::string>{""};
    if (!text) return JS_EXCEPTION;
    return detail::wrapWidget(ctx, target, new lcl::ui::TextField(*text));
}

JSValue imageCtor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    const auto path = argc > 0 ? stringValue(ctx, argv[0]) : std::optional<std::string>{""};
    if (!path) return JS_EXCEPTION;
    return detail::wrapWidget(ctx, target, new lcl::ui::Image(*path));
}

JSValue scrollViewCtor(JSContext* ctx, JSValueConst target, int, JSValueConst*) {
    return detail::wrapWidget(ctx, target, new lcl::ui::ScrollView());
}

JSValue toggleCtor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    if (argc > 0 && JS_IsString(argv[0])) {
        const auto label = stringValue(ctx, argv[0]);
        if (!label) return JS_EXCEPTION;
        const bool value = argc > 1 && JS_ToBool(ctx, argv[1]) != 0;
        return detail::wrapWidget(ctx, target, new lcl::ui::Toggle(*label, value));
    }
    const bool value = argc > 0 && JS_ToBool(ctx, argv[0]) != 0;
    return detail::wrapWidget(ctx, target, new lcl::ui::Toggle(value));
}

JSValue sliderCtor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    double value = 0.0, minimum = 0.0, maximum = 1.0, step = 0.0;
    std::array<double*, 4> values{&value, &minimum, &maximum, &step};
    for (int index = 0; index < argc && index < static_cast<int>(values.size()); ++index) {
        if (!numberValue(ctx, argv[index], *values[index]))
            return JS_ThrowTypeError(ctx, "Slider arguments must be finite numbers");
    }
    return detail::wrapWidget(ctx, target, new lcl::ui::Slider(
        static_cast<float>(value), static_cast<float>(minimum),
        static_cast<float>(maximum), static_cast<float>(step)));
}

JSValue progressCtor(JSContext* ctx, JSValueConst target, int argc, JSValueConst* argv) {
    if (argc == 0 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]))
        return detail::wrapWidget(ctx, target, new lcl::ui::ProgressView());
    double value = 0.0, total = 1.0;
    if (!numberValue(ctx, argv[0], value) ||
        (argc > 1 && !numberValue(ctx, argv[1], total)))
        return JS_ThrowTypeError(ctx, "ProgressView value and total must be finite numbers");
    return detail::wrapWidget(ctx, target, new lcl::ui::ProgressView(
        static_cast<float>(value), static_cast<float>(total)));
}

JSValue tabViewCtor(JSContext* ctx, JSValueConst target, int, JSValueConst*) {
    return detail::wrapWidget(ctx, target, new lcl::ui::TabView());
}

JSValue containerSetBackground(JSContext* ctx, JSValueConst self, int argc,
                               JSValueConst* argv) {
    auto* target = control<lcl::ui::Container>(ctx, self, "Container");
    if (!target) return JS_EXCEPTION;
    lcl::graphics::Color color;
    if (!colorValue(ctx, argc, argv, color))
        return JS_ThrowTypeError(ctx, "setBackgroundColor requires a color");
    target->setBackgroundColor(color);
    return JS_UNDEFINED;
}
JSValue containerSetBorderColor(JSContext* ctx, JSValueConst self, int argc,
                                JSValueConst* argv) {
    auto* target = control<lcl::ui::Container>(ctx, self, "Container");
    if (!target) return JS_EXCEPTION;
    lcl::graphics::Color color;
    if (!colorValue(ctx, argc, argv, color))
        return JS_ThrowTypeError(ctx, "setBorderColor requires a color");
    target->setBorderColor(color);
    return JS_UNDEFINED;
}
JSValue containerSetBorderWidth(JSContext* c, JSValueConst s, int a,
                                JSValueConst* v) {
    return setNumber<lcl::ui::Container>(c, s, a, v, "setBorderWidth",
        [](auto& target, float value) { target.setBorderWidth(std::max(0.0f, value)); });
}
JSValue containerSetBorderRadius(JSContext* ctx, JSValueConst self, int argc,
                                 JSValueConst* argv) {
    auto* target = control<lcl::ui::Container>(ctx, self, "Container");
    if (!target) return JS_EXCEPTION;
    double radius = 0.0, roundness = 3.2;
    if (argc < 1 || !numberValue(ctx, argv[0], radius) ||
        (argc > 1 && !numberValue(ctx, argv[1], roundness))) {
        return JS_ThrowTypeError(ctx, "setBorderRadius requires finite values");
    }
    target->setBorderRadius(static_cast<float>(std::max(0.0, radius)));
    if (argc > 1) target->setBorderRoundness(static_cast<float>(roundness));
    return JS_UNDEFINED;
}
JSValue containerSetBorderRoundness(JSContext* c, JSValueConst s, int a,
                                    JSValueConst* v) {
    return setNumber<lcl::ui::Container>(c, s, a, v, "setBorderRoundness",
        [](auto& target, float value) { target.setBorderRoundness(value); });
}

JSValue buttonSetLabel(JSContext* ctx, JSValueConst self, int argc,
                       JSValueConst* argv) {
    auto* target = control<lcl::ui::Button>(ctx, self, "Button");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "label is required");
    target->setLabel(*value);
    return JS_UNDEFINED;
}
JSValue buttonGetLabel(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Button>(ctx, self, "Button");
    return target ? JS_NewString(ctx, target->getLabel().c_str()) : JS_EXCEPTION;
}

JSValue textSetText(JSContext* ctx, JSValueConst self, int argc,
                    JSValueConst* argv) {
    auto* target = control<lcl::ui::Text>(ctx, self, "Text");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "text is required");
    target->setText(*value);
    return JS_UNDEFINED;
}
JSValue textGetText(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Text>(ctx, self, "Text");
    return target ? JS_NewString(ctx, target->getText().c_str()) : JS_EXCEPTION;
}
JSValue textSetFontSize(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::Text>(c, s, a, v, "setFontSize",
        [](auto& target, float value) { target.setFontSize(value); });
}
JSValue textSetColor(JSContext* ctx, JSValueConst self, int argc,
                     JSValueConst* argv) {
    auto* target = control<lcl::ui::Text>(ctx, self, "Text");
    if (!target) return JS_EXCEPTION;
    lcl::graphics::Color color;
    if (!colorValue(ctx, argc, argv, color))
        return JS_ThrowTypeError(ctx, "setTextColor requires a color");
    target->setTextColor(color);
    return JS_UNDEFINED;
}

JSValue setTopOnlyRadius(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setBool<lcl::ui::Container>(c, s, a, v, "setTopOnlyBorderRadius",
        [](auto& target, bool value) { target.setTopOnlyBorderRadius(value); });
}
JSValue buttonSetEnabled(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setBool<lcl::ui::Button>(c, s, a, v, "setEnabled",
        [](auto& target, bool value) { target.setEnabled(value); });
}
JSValue buttonIsEnabled(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Button>(ctx, self, "Button");
    return target ? JS_NewBool(ctx, target->isEnabled()) : JS_EXCEPTION;
}

JSValue textSetRole(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::Text>(ctx, self, "Text"); if (!target) return JS_EXCEPTION;
    const auto role = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!role) return JS_ThrowTypeError(ctx, "text role is required");
    if (*role == "title") target->setTextRole(lcl::theme::TextRole::Title);
    else if (*role == "body") target->setTextRole(lcl::theme::TextRole::Body);
    else if (*role == "description") target->setTextRole(lcl::theme::TextRole::Description);
    else if (*role == "caption") target->setTextRole(lcl::theme::TextRole::Caption);
    else return JS_ThrowRangeError(ctx, "unknown text role");
    return JS_UNDEFINED;
}

JSValue textSetAlign(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::Text>(ctx, self, "Text"); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "text alignment is required");
    if (*value == "left" || *value == "start") target->setTextAlign(lcl::ui::TextAlign::Start);
    else if (*value == "center") target->setTextAlign(lcl::ui::TextAlign::Center);
    else if (*value == "right" || *value == "end") target->setTextAlign(lcl::ui::TextAlign::End);
    else return JS_ThrowRangeError(ctx, "unknown text alignment");
    return JS_UNDEFINED;
}

JSValue textResetFontSize(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Text>(ctx, self, "Text"); if (!target) return JS_EXCEPTION;
    target->resetFontSize(); return JS_UNDEFINED;
}
JSValue textResetColor(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Text>(ctx, self, "Text"); if (!target) return JS_EXCEPTION;
    target->resetTextColor(); return JS_UNDEFINED;
}

std::optional<lcl::protocol::FilterType> filterType(std::string_view name) {
    using Type = lcl::protocol::FilterType;
    if (name == "blur") return Type::Blur;
    if (name == "brightness") return Type::Brightness;
    if (name == "contrast") return Type::Contrast;
    if (name == "saturation") return Type::Saturation;
    if (name == "grayscale") return Type::Grayscale;
    if (name == "invert") return Type::Invert;
    if (name == "glass") return Type::Glass;
    return std::nullopt;
}

template <typename Type>
JSValue addFilter(JSContext* ctx, JSValueConst self, int argc,
                  JSValueConst* argv, const char* name) {
    auto* target = control<Type>(ctx, self, name);
    if (!target) return JS_EXCEPTION;
    const auto typeName = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto type = typeName ? filterType(*typeName) : std::nullopt;
    double value = 0.0, parameter1 = 1.4, parameter2 = 7.0;
    if (!type || argc < 2 || !numberValue(ctx, argv[1], value) ||
        (argc > 2 && !numberValue(ctx, argv[2], parameter1)) ||
        (argc > 3 && !numberValue(ctx, argv[3], parameter2))) {
        return JS_ThrowTypeError(ctx, "addFilter requires a known filter and finite values");
    }
    if (*type == lcl::protocol::FilterType::Glass) {
        if constexpr (std::is_same_v<Type, lcl::ui::BackdropSurface>) {
            target->addFilter(*type, static_cast<float>(std::max(0.0, value)),
                              static_cast<float>(std::max(0.0, parameter1)),
                              static_cast<float>(std::max(0.0, parameter2)));
        } else {
            target->addFilter(*type, static_cast<float>(std::max(0.0, value)));
        }
    } else {
        target->addFilter(*type, static_cast<float>(value));
    }
    return JS_UNDEFINED;
}

JSValue backdropAddFilter(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return addFilter<lcl::ui::BackdropSurface>(c, s, a, v, "BackdropSurface");
}
JSValue backdropClearFilters(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::BackdropSurface>(ctx, self, "BackdropSurface");
    if (!target) return JS_EXCEPTION;
    target->clearFilters();
    return JS_UNDEFINED;
}
JSValue backdropSetTint(JSContext* ctx, JSValueConst self, int argc,
                        JSValueConst* argv) {
    auto* target = control<lcl::ui::BackdropSurface>(ctx, self, "BackdropSurface");
    if (!target) return JS_EXCEPTION;
    lcl::graphics::Color color;
    if (!colorValue(ctx, argc, argv, color))
        return JS_ThrowTypeError(ctx, "setTint requires a color");
    target->setTint(color);
    return JS_UNDEFINED;
}
JSValue backdropSetEffectBounds(JSContext* ctx, JSValueConst self, int argc,
                                JSValueConst* argv) {
    auto* target = control<lcl::ui::BackdropSurface>(ctx, self, "BackdropSurface");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "effect bounds is required");
    if (*value == "local") target->setEffectBounds(lcl::ui::EffectBounds::Local);
    else if (*value == "outer-surface" || *value == "outerSurface")
        target->setEffectBounds(lcl::ui::EffectBounds::OuterSurface);
    else return JS_ThrowRangeError(ctx, "effect bounds must be local or outer-surface");
    return JS_UNDEFINED;
}
JSValue backdropSetInteractive(JSContext* c, JSValueConst s, int a,
                               JSValueConst* v) {
    return setBool<lcl::ui::BackdropSurface>(c, s, a, v, "setInteractive",
        [](auto& target, bool value) { target.setInteractive(value); });
}
JSValue backdropSetBlend(JSContext* ctx, JSValueConst self, int argc,
                         JSValueConst* argv) {
    auto* target = control<lcl::ui::BackdropSurface>(ctx, self, "BackdropSurface");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "blend mode is required");
    if (*value == "normal") target->setBlendMode(lcl::ui::EffectBlend::Normal);
    else if (*value == "screen") target->setBlendMode(lcl::ui::EffectBlend::Screen);
    else if (*value == "multiply") target->setBlendMode(lcl::ui::EffectBlend::Multiply);
    else if (*value == "overlay") target->setBlendMode(lcl::ui::EffectBlend::Overlay);
    else if (*value == "plus") target->setBlendMode(lcl::ui::EffectBlend::Plus);
    else return JS_ThrowRangeError(ctx, "unknown blend mode");
    return JS_UNDEFINED;
}

JSValue textFieldSetText(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::TextField>(ctx, self, "TextField"); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "text is required");
    target->setText(*value); return JS_UNDEFINED;
}
JSValue textFieldGetText(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::TextField>(ctx, self, "TextField");
    return target ? JS_NewString(ctx, target->getText().c_str()) : JS_EXCEPTION;
}
JSValue textFieldSetPlaceholder(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::TextField>(ctx, self, "TextField"); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "placeholder is required");
    target->setPlaceholder(*value); return JS_UNDEFINED;
}
JSValue textFieldGetPlaceholder(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::TextField>(ctx, self, "TextField");
    return target ? JS_NewString(ctx, target->getPlaceholder().c_str()) : JS_EXCEPTION;
}
JSValue textFieldOnChange(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::TextField>(ctx, self, "TextField"); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setOnChange requires a function or null");
    auto store = detail::setWidgetCallback(ctx, self, "textField.change", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnChange(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? lcl::ui::TextField::ChangeCallback{}
        : lcl::ui::TextField::ChangeCallback([store](const std::string& value) {
            detail::invokeStringCallback(store, "textField.change", value);
        }));
    return JS_UNDEFINED;
}

JSValue scrollSetContent(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::ScrollView>(ctx, self, "ScrollView"); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "content widget is required");
    auto content = detail::takeWidget(ctx, argv[0]); if (!content) return JS_EXCEPTION;
    target->setContent(std::move(content)); return JS_UNDEFINED;
}
JSValue scrollSetY(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::ScrollView>(c, s, a, v, "setScrollY",
        [](auto& target, float value) { target.setScrollY(value); });
}
JSValue scrollGetY(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::ScrollView>(ctx, self, "ScrollView");
    return target ? JS_NewFloat64(ctx, target->getScrollY()) : JS_EXCEPTION;
}
JSValue scrollGetMaxY(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::ScrollView>(ctx, self, "ScrollView");
    return target ? JS_NewFloat64(ctx, target->getMaxScrollY()) : JS_EXCEPTION;
}
JSValue scrollSetSpeed(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::ScrollView>(c, s, a, v, "setScrollSpeed",
        [](auto& target, float value) { target.setScrollSpeed(value); });
}

JSValue imageSetSource(JSContext* ctx, JSValueConst self, int argc,
                       JSValueConst* argv) {
    auto* target = control<lcl::ui::Image>(ctx, self, "Image");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "source path is required");
    return JS_NewBool(ctx, target->setSourcePath(*value));
}
JSValue imageGetSource(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Image>(ctx, self, "Image");
    return target ? JS_NewString(ctx, target->getSourcePath().c_str()) : JS_EXCEPTION;
}
JSValue imageClearSource(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Image>(ctx, self, "Image");
    if (!target) return JS_EXCEPTION;
    target->clearSource();
    return JS_UNDEFINED;
}
JSValue imageSetFit(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::Image>(ctx, self, "Image");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "image fit is required");
    if (*value == "fill") target->setFit(lcl::ui::ImageFit::Fill);
    else if (*value == "contain") target->setFit(lcl::ui::ImageFit::Contain);
    else if (*value == "cover") target->setFit(lcl::ui::ImageFit::Cover);
    else return JS_ThrowRangeError(ctx, "image fit must be fill, contain, or cover");
    return JS_UNDEFINED;
}
JSValue imageSetCornerRadius(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::Image>(c, s, a, v, "setCornerRadius",
        [](auto& target, float value) { target.setCornerRadius(std::max(0.0f, value)); });
}
JSValue imageSetCornerRoundness(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::Image>(c, s, a, v, "setCornerRoundness",
        [](auto& target, float value) { target.setCornerRoundness(value); });
}
JSValue imageSetOpacity(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::Image>(c, s, a, v, "setOpacity",
        [](auto& target, float value) { target.setOpacity(std::clamp(value, 0.0f, 1.0f)); });
}

JSValue toggleSetValue(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setBool<lcl::ui::Toggle>(c, s, a, v, "setValue",
        [](auto& target, bool value) { target.setValue(value); });
}
JSValue toggleValue(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Toggle>(ctx, self, "Toggle");
    return target ? JS_NewBool(ctx, target->value()) : JS_EXCEPTION;
}
JSValue toggleSetMixed(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setBool<lcl::ui::Toggle>(c, s, a, v, "setMixed",
        [](auto& target, bool value) { target.setMixed(value); });
}
JSValue toggleSetEnabled(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setBool<lcl::ui::Toggle>(c, s, a, v, "setEnabled",
        [](auto& target, bool value) { target.setEnabled(value); });
}
JSValue toggleSetLabel(JSContext* ctx, JSValueConst self, int argc,
                       JSValueConst* argv) {
    auto* target = control<lcl::ui::Toggle>(ctx, self, "Toggle");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "label is required");
    target->setLabel(*value);
    return JS_UNDEFINED;
}
JSValue toggleGetLabel(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Toggle>(ctx, self, "Toggle");
    return target ? JS_NewString(ctx, target->label().c_str()) : JS_EXCEPTION;
}
JSValue toggleIsMixed(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Toggle>(ctx, self, "Toggle");
    return target ? JS_NewBool(ctx, target->isMixed()) : JS_EXCEPTION;
}
JSValue toggleIsEnabled(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Toggle>(ctx, self, "Toggle");
    return target ? JS_NewBool(ctx, target->isEnabled()) : JS_EXCEPTION;
}
JSValue toggleSetStyle(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::Toggle>(ctx, self, "Toggle"); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "toggle style is required");
    if (*value == "automatic") target->setToggleStyle(lcl::ui::ToggleStyle::Automatic);
    else if (*value == "switch") target->setToggleStyle(lcl::ui::ToggleStyle::Switch);
    else if (*value == "checkbox") target->setToggleStyle(lcl::ui::ToggleStyle::Checkbox);
    else if (*value == "button") target->setToggleStyle(lcl::ui::ToggleStyle::Button);
    else return JS_ThrowRangeError(ctx, "unknown toggle style");
    return JS_UNDEFINED;
}
JSValue toggleOnChange(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::Toggle>(ctx, self, "Toggle"); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setOnChange requires a function or null");
    auto store = detail::setWidgetCallback(ctx, self, "toggle.change", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnChange(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? std::function<void(bool)>{}
        : std::function<void(bool)>([store](bool value) {
            detail::invokeBoolCallback(store, "toggle.change", value);
        }));
    return JS_UNDEFINED;
}

JSValue sliderSetValue(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::Slider>(c, s, a, v, "setValue",
        [](auto& target, float value) { target.setValue(value); });
}
JSValue sliderValue(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider");
    return target ? JS_NewFloat64(ctx, target->value()) : JS_EXCEPTION;
}
JSValue sliderSetRange(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider"); if (!target) return JS_EXCEPTION;
    double minimum = 0.0, maximum = 0.0;
    if (argc < 2 || !numberValue(ctx, argv[0], minimum) || !numberValue(ctx, argv[1], maximum))
        return JS_ThrowTypeError(ctx, "setRange requires minimum and maximum");
    target->setRange(static_cast<float>(minimum), static_cast<float>(maximum)); return JS_UNDEFINED;
}
JSValue sliderSetStep(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::Slider>(c, s, a, v, "setStep",
        [](auto& target, float value) { target.setStep(value); });
}
JSValue sliderSetEnabled(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setBool<lcl::ui::Slider>(c, s, a, v, "setEnabled",
        [](auto& target, bool value) { target.setEnabled(value); });
}
JSValue sliderGetMinimum(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider");
    return target ? JS_NewFloat64(ctx, target->minimum()) : JS_EXCEPTION;
}
JSValue sliderGetMaximum(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider");
    return target ? JS_NewFloat64(ctx, target->maximum()) : JS_EXCEPTION;
}
JSValue sliderGetStep(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider");
    return target ? JS_NewFloat64(ctx, target->step()) : JS_EXCEPTION;
}
JSValue sliderIsEnabled(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider");
    return target ? JS_NewBool(ctx, target->isEnabled()) : JS_EXCEPTION;
}
JSValue sliderOnChange(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider");
    if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setOnChange requires a function or null");
    auto store = detail::setWidgetCallback(ctx, self, "slider.change", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnChange(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? lcl::ui::Slider::ChangeCallback{}
        : lcl::ui::Slider::ChangeCallback([store](float value) {
            detail::invokeNumberCallback(store, "slider.change", value);
        }));
    return JS_UNDEFINED;
}
JSValue sliderOnEditingChanged(JSContext* ctx, JSValueConst self, int argc,
                               JSValueConst* argv) {
    auto* target = control<lcl::ui::Slider>(ctx, self, "Slider");
    if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setOnEditingChanged requires a function or null");
    auto store = detail::setWidgetCallback(ctx, self, "slider.editing", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnEditingChanged(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? lcl::ui::Slider::EditingCallback{}
        : lcl::ui::Slider::EditingCallback([store](bool editing) {
            detail::invokeBoolCallback(store, "slider.editing", editing);
        }));
    return JS_UNDEFINED;
}

JSValue progressSetValue(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::ProgressView>(ctx, self, "ProgressView"); if (!target) return JS_EXCEPTION;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        target->setValue(std::nullopt); return JS_UNDEFINED;
    }
    double value = 0.0;
    if (!numberValue(ctx, argv[0], value)) return JS_ThrowTypeError(ctx, "progress value must be finite or null");
    target->setValue(static_cast<float>(value)); return JS_UNDEFINED;
}
JSValue progressSetTotal(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::ProgressView>(c, s, a, v, "setTotal",
        [](auto& target, float value) { target.setTotal(value); });
}
JSValue progressSetStyle(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::ProgressView>(ctx, self, "ProgressView"); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "progress style is required");
    if (*value == "automatic") target->setProgressViewStyle(lcl::ui::ProgressViewStyle::Automatic);
    else if (*value == "linear") target->setProgressViewStyle(lcl::ui::ProgressViewStyle::Linear);
    else if (*value == "circular") target->setProgressViewStyle(lcl::ui::ProgressViewStyle::Circular);
    else return JS_ThrowRangeError(ctx, "unknown progress style");
    return JS_UNDEFINED;
}
JSValue progressGetValue(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::ProgressView>(ctx, self, "ProgressView");
    if (!target) return JS_EXCEPTION;
    const auto value = target->value();
    return value ? JS_NewFloat64(ctx, *value) : JS_NULL;
}
JSValue progressGetTotal(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::ProgressView>(ctx, self, "ProgressView");
    return target ? JS_NewFloat64(ctx, target->total()) : JS_EXCEPTION;
}

JSValue tabAdd(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::TabView>(ctx, self, "TabView"); if (!target) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "addTab requires title and content");
    const auto title = stringValue(ctx, argv[0]); if (!title) return JS_EXCEPTION;
    auto content = detail::takeWidget(ctx, argv[1]); if (!content) return JS_EXCEPTION;
    const bool enabled = argc < 3 || JS_ToBool(ctx, argv[2]) != 0;
    target->addTab({*title, std::move(content), enabled}); return JS_UNDEFINED;
}
JSValue tabSetSelected(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::TabView>(ctx, self, "TabView"); if (!target) return JS_EXCEPTION;
    int64_t value = 0;
    if (argc < 1 || JS_ToInt64(ctx, &value, argv[0]) < 0 || value < 0)
        return JS_ThrowRangeError(ctx, "selected index must be non-negative");
    target->setSelectedIndex(static_cast<size_t>(value)); return JS_UNDEFINED;
}
JSValue tabSelected(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::TabView>(ctx, self, "TabView");
    return target ? JS_NewInt64(ctx, static_cast<int64_t>(target->selectedIndex())) : JS_EXCEPTION;
}
JSValue tabCount(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::TabView>(ctx, self, "TabView");
    return target ? JS_NewInt64(ctx, static_cast<int64_t>(target->tabCount())) : JS_EXCEPTION;
}
JSValue tabOnChange(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::TabView>(ctx, self, "TabView");
    if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setOnChange requires a function or null");
    auto store = detail::setWidgetCallback(ctx, self, "tab.change", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnChange(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? lcl::ui::TabView::ChangeCallback{}
        : lcl::ui::TabView::ChangeCallback([store](size_t index) {
            detail::invokeNumberCallback(store, "tab.change", static_cast<double>(index));
        }));
    return JS_UNDEFINED;
}
JSValue tabSetStyle(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::TabView>(ctx, self, "TabView");
    if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "tab view style is required");
    if (*value == "automatic") target->setTabViewStyle(lcl::ui::TabViewStyle::Automatic);
    else if (*value == "tab-bar" || *value == "tabBar") target->setTabViewStyle(lcl::ui::TabViewStyle::TabBar);
    else return JS_ThrowRangeError(ctx, "unknown tab view style");
    return JS_UNDEFINED;
}

JSValue filterSetBlend(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = control<lcl::ui::FilterGroup>(ctx, self, "FilterGroup"); if (!target) return JS_EXCEPTION;
    const auto value = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!value) return JS_ThrowTypeError(ctx, "blend mode is required");
    if (*value == "normal") target->setBlendMode(lcl::ui::EffectBlend::Normal);
    else if (*value == "screen") target->setBlendMode(lcl::ui::EffectBlend::Screen);
    else if (*value == "multiply") target->setBlendMode(lcl::ui::EffectBlend::Multiply);
    else if (*value == "overlay") target->setBlendMode(lcl::ui::EffectBlend::Overlay);
    else if (*value == "plus") target->setBlendMode(lcl::ui::EffectBlend::Plus);
    else return JS_ThrowRangeError(ctx, "unknown blend mode");
    return JS_UNDEFINED;
}
JSValue filterSetOpacity(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setNumber<lcl::ui::FilterGroup>(c, s, a, v, "setOpacity",
        [](auto& target, float value) { target.setOpacity(value); });
}
JSValue filterAddFilter(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return addFilter<lcl::ui::FilterGroup>(c, s, a, v, "FilterGroup");
}
JSValue filterClearFilters(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = control<lcl::ui::FilterGroup>(ctx, self, "FilterGroup");
    if (!target) return JS_EXCEPTION;
    target->clearFilters();
    return JS_UNDEFINED;
}

void method(JSContext* ctx, JSValue proto, const char* name, JSCFunction* fn, int argc) {
    detail::addMethod(ctx, proto, name, fn, argc);
}

JSValue prototype(JSContext* ctx, JSValue base,
                  std::initializer_list<std::tuple<const char*, JSCFunction*, int>> methods) {
    JSValue result = detail::makeDerivedPrototype(ctx, base);
    for (const auto& [name, function, argc] : methods) method(ctx, result, name, function, argc);
    return result;
}

} // namespace

void registerControlBindings(JSContext* ctx, JSValue widgetProto, JSValue lcl) {
    JSValue container = prototype(ctx, widgetProto, {
        {"setBackgroundColor", containerSetBackground, 4},
        {"setBorderColor", containerSetBorderColor, 4},
        {"setBorderWidth", containerSetBorderWidth, 1},
        {"setBorderRadius", containerSetBorderRadius, 2},
        {"setBorderRoundness", containerSetBorderRoundness, 1},
        {"setTopOnlyBorderRadius", setTopOnlyRadius, 1},
    });
    detail::addConstructor(ctx, lcl, "Container", containerCtor, 0, container);

    JSValue focus = prototype(ctx, container, {});
    detail::addConstructor(ctx, lcl, "FocusScope", focusScopeCtor, 0, focus);

    JSValue button = prototype(ctx, container, {
        {"setLabel", buttonSetLabel, 1}, {"getLabel", buttonGetLabel, 0},
        {"setEnabled", buttonSetEnabled, 1}, {"isEnabled", buttonIsEnabled, 0},
    });
    detail::addConstructor(ctx, lcl, "Button", buttonCtor, 1, button);

    JSValue backdrop = prototype(ctx, container, {
        {"addFilter", backdropAddFilter, 4}, {"clearFilters", backdropClearFilters, 0},
        {"setTint", backdropSetTint, 4}, {"setEffectBounds", backdropSetEffectBounds, 1},
        {"setInteractive", backdropSetInteractive, 1}, {"setBlendMode", backdropSetBlend, 1},
    });
    detail::addConstructor(ctx, lcl, "BackdropSurface", backdropCtor, 0, backdrop);

    JSValue text = prototype(ctx, widgetProto, {
        {"setText", textSetText, 1}, {"getText", textGetText, 0},
        {"setFontSize", textSetFontSize, 1}, {"setTextColor", textSetColor, 4},
        {"setTextRole", textSetRole, 1}, {"setTextAlign", textSetAlign, 1},
        {"resetFontSize", textResetFontSize, 0}, {"resetTextColor", textResetColor, 0},
    });
    detail::addConstructor(ctx, lcl, "Text", textCtor, 1, text);

    JSValue field = prototype(ctx, widgetProto, {{"setText", textFieldSetText, 1}, {"getText", textFieldGetText, 0},
        {"setPlaceholder", textFieldSetPlaceholder, 1}, {"getPlaceholder", textFieldGetPlaceholder, 0},
        {"setOnChange", textFieldOnChange, 1}});
    detail::addConstructor(ctx, lcl, "TextField", textFieldCtor, 1, field);

    JSValue image = prototype(ctx, widgetProto, {
        {"setSourcePath", imageSetSource, 1}, {"getSourcePath", imageGetSource, 0},
        {"clearSource", imageClearSource, 0}, {"setFit", imageSetFit, 1},
        {"setCornerRadius", imageSetCornerRadius, 1},
        {"setCornerRoundness", imageSetCornerRoundness, 1},
        {"setOpacity", imageSetOpacity, 1},
    });
    detail::addConstructor(ctx, lcl, "Image", imageCtor, 1, image);

    JSValue scroll = prototype(ctx, widgetProto, {{"setContent", scrollSetContent, 1}, {"setScrollY", scrollSetY, 1},
        {"getScrollY", scrollGetY, 0}, {"getMaxScrollY", scrollGetMaxY, 0}, {"setScrollSpeed", scrollSetSpeed, 1}});
    detail::addConstructor(ctx, lcl, "ScrollView", scrollViewCtor, 0, scroll);

    JSValue toggle = prototype(ctx, widgetProto, {
        {"setValue", toggleSetValue, 1}, {"getValue", toggleValue, 0},
        {"setLabel", toggleSetLabel, 1}, {"getLabel", toggleGetLabel, 0},
        {"setMixed", toggleSetMixed, 1}, {"isMixed", toggleIsMixed, 0},
        {"setEnabled", toggleSetEnabled, 1}, {"isEnabled", toggleIsEnabled, 0},
        {"setToggleStyle", toggleSetStyle, 1}, {"setOnChange", toggleOnChange, 1},
    });
    detail::addConstructor(ctx, lcl, "Toggle", toggleCtor, 2, toggle);

    JSValue slider = prototype(ctx, widgetProto, {
        {"setValue", sliderSetValue, 1}, {"getValue", sliderValue, 0},
        {"setRange", sliderSetRange, 2}, {"getMinimum", sliderGetMinimum, 0},
        {"getMaximum", sliderGetMaximum, 0}, {"setStep", sliderSetStep, 1},
        {"getStep", sliderGetStep, 0}, {"setEnabled", sliderSetEnabled, 1},
        {"isEnabled", sliderIsEnabled, 0}, {"setOnChange", sliderOnChange, 1},
        {"setOnEditingChanged", sliderOnEditingChanged, 1},
    });
    detail::addConstructor(ctx, lcl, "Slider", sliderCtor, 4, slider);

    JSValue progress = prototype(ctx, widgetProto, {
        {"setValue", progressSetValue, 1}, {"getValue", progressGetValue, 0},
        {"setTotal", progressSetTotal, 1}, {"getTotal", progressGetTotal, 0},
        {"setProgressViewStyle", progressSetStyle, 1},
    });
    detail::addConstructor(ctx, lcl, "ProgressView", progressCtor, 2, progress);

    JSValue divider = prototype(ctx, widgetProto, {});
    detail::addConstructor(ctx, lcl, "Divider", dividerCtor, 0, divider);

    JSValue tabs = prototype(ctx, widgetProto, {
        {"addTab", tabAdd, 3}, {"setSelectedIndex", tabSetSelected, 1},
        {"getSelectedIndex", tabSelected, 0}, {"getTabCount", tabCount, 0},
        {"setOnChange", tabOnChange, 1}, {"setTabViewStyle", tabSetStyle, 1},
    });
    detail::addConstructor(ctx, lcl, "TabView", tabViewCtor, 0, tabs);

    JSValue filters = prototype(ctx, container, {
        {"addFilter", filterAddFilter, 2}, {"clearFilters", filterClearFilters, 0},
        {"setBlendMode", filterSetBlend, 1}, {"setOpacity", filterSetOpacity, 1},
    });
    detail::addConstructor(ctx, lcl, "FilterGroup", filterGroupCtor, 0, filters);
}

} // namespace lcl::binding
