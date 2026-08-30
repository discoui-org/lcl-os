#include "system/javascript/js_binding_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "system/javascript/js_runtime.hpp"
#include "lcl-theme/theme.hpp"
#include "lcl-ui/layout/layout.hpp"
#include "lcl-ui/core/animation.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::binding {
namespace {

namespace layout = lcl::ui::layout;

lcl::ui::Widget* widget(JSContext* ctx, JSValueConst value) {
    return detail::requireWidget(ctx, value);
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

std::optional<layout::Direction> direction(std::string_view value) {
    if (value == "row") return layout::Direction::Row;
    if (value == "row-reverse") return layout::Direction::RowReverse;
    if (value == "column") return layout::Direction::Column;
    if (value == "column-reverse") return layout::Direction::ColumnReverse;
    return std::nullopt;
}

std::optional<layout::Justify> justify(std::string_view value) {
    if (value == "start" || value == "flex-start") return layout::Justify::FlexStart;
    if (value == "center") return layout::Justify::Center;
    if (value == "end" || value == "flex-end") return layout::Justify::FlexEnd;
    if (value == "space-between") return layout::Justify::SpaceBetween;
    if (value == "space-around") return layout::Justify::SpaceAround;
    if (value == "space-evenly") return layout::Justify::SpaceEvenly;
    return std::nullopt;
}

std::optional<layout::Align> align(std::string_view value) {
    if (value == "auto") return layout::Align::Auto;
    if (value == "start" || value == "flex-start") return layout::Align::FlexStart;
    if (value == "center") return layout::Align::Center;
    if (value == "end" || value == "flex-end") return layout::Align::FlexEnd;
    if (value == "stretch") return layout::Align::Stretch;
    if (value == "baseline") return layout::Align::Baseline;
    if (value == "space-between") return layout::Align::SpaceBetween;
    if (value == "space-around") return layout::Align::SpaceAround;
    return std::nullopt;
}

std::optional<layout::Edge> edge(std::string_view value) {
    if (value == "left") return layout::Edge::Left;
    if (value == "top") return layout::Edge::Top;
    if (value == "right") return layout::Edge::Right;
    if (value == "bottom") return layout::Edge::Bottom;
    if (value == "start") return layout::Edge::Start;
    if (value == "end") return layout::Edge::End;
    if (value == "horizontal") return layout::Edge::Horizontal;
    if (value == "vertical") return layout::Edge::Vertical;
    if (value == "all") return layout::Edge::All;
    return std::nullopt;
}

std::optional<layout::Gutter> gutter(std::string_view value) {
    if (value == "column") return layout::Gutter::Column;
    if (value == "row") return layout::Gutter::Row;
    if (value == "all") return layout::Gutter::All;
    return std::nullopt;
}

std::optional<lcl::ui::EffectSource> effectSource(std::string_view value) {
    if (value == "layer") return lcl::ui::EffectSource::Layer;
    if (value == "backdrop") return lcl::ui::EffectSource::Backdrop;
    if (value == "surface-backdrop" || value == "surfaceBackdrop")
        return lcl::ui::EffectSource::SurfaceBackdrop;
    return std::nullopt;
}

std::optional<lcl::ui::EffectType> effectType(std::string_view value) {
    using Type = lcl::ui::EffectType;
    if (value == "blur") return Type::Blur;
    if (value == "brightness") return Type::Brightness;
    if (value == "contrast") return Type::Contrast;
    if (value == "saturation") return Type::Saturation;
    if (value == "grayscale") return Type::Grayscale;
    if (value == "invert") return Type::Invert;
    if (value == "glass") return Type::Glass;
    if (value == "tint") return Type::Tint;
    return std::nullopt;
}

template <typename Setter>
JSValue numericSetter(JSContext* ctx, JSValueConst thisValue, int argc,
                      JSValueConst* argv, const char* name, Setter setter) {
    auto* target = widget(ctx, thisValue);
    if (!target) return JS_EXCEPTION;
    double value = 0.0;
    if (argc < 1 || !numberValue(ctx, argv[0], value))
        return JS_ThrowTypeError(ctx, "%s requires a finite number", name);
    setter(*target, static_cast<float>(value));
    return JS_UNDEFINED;
}

JSValue js_set_width_auto(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    target->setWidthAuto(); return JS_UNDEFINED;
}
JSValue js_set_width(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setWidth", [](auto& w, float n) { w.setWidth(n); });
}
JSValue js_set_height(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setHeight", [](auto& w, float n) { w.setHeight(n); });
}
JSValue js_set_height_auto(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    target->setHeightAuto(); return JS_UNDEFINED;
}
JSValue js_set_min_width(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setMinWidth", [](auto& w, float n) { w.setMinWidth(n); });
}
JSValue js_set_min_height(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setMinHeight", [](auto& w, float n) { w.setMinHeight(n); });
}
JSValue js_set_max_width(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setMaxWidth", [](auto& w, float n) { w.setMaxWidth(n); });
}
JSValue js_set_max_height(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setMaxHeight", [](auto& w, float n) { w.setMaxHeight(n); });
}
JSValue js_set_flex_grow(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setFlexGrow", [](auto& w, float n) { w.setFlexGrow(n); });
}
JSValue js_set_flex_shrink(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setFlexShrink", [](auto& w, float n) { w.setFlexShrink(n); });
}
JSValue js_set_flex_basis(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setFlexBasis", [](auto& w, float n) { w.setFlexBasis(n); });
}
JSValue js_set_flex_basis_auto(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    target->setFlexBasisAuto(); return JS_UNDEFINED;
}
JSValue js_set_translation_x(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setTranslationX", [](auto& w, float n) { w.setTranslationX(n); });
}
JSValue js_set_translation_y(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setTranslationY", [](auto& w, float n) { w.setTranslationY(n); });
}
JSValue js_set_opacity(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setOpacity", [](auto& w, float n) { w.setOpacity(n); });
}
JSValue js_set_rotation(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return numericSetter(c, s, a, v, "setRotation", [](auto& w, float n) { w.setRotation(n); });
}
JSValue js_set_translation(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    double x = 0.0, y = 0.0;
    if (argc < 2 || !numberValue(ctx, argv[0], x) || !numberValue(ctx, argv[1], y))
        return JS_ThrowTypeError(ctx, "setTranslation requires x and y");
    target->setTranslation(static_cast<float>(x), static_cast<float>(y)); return JS_UNDEFINED;
}
JSValue js_set_scale(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    double x = 1.0, y = 1.0;
    if (argc < 1 || !numberValue(ctx, argv[0], x) ||
        (argc > 1 && !numberValue(ctx, argv[1], y)))
        return JS_ThrowTypeError(ctx, "setScale requires one or two finite numbers");
    if (argc == 1) y = x;
    target->setScale(static_cast<float>(x), static_cast<float>(y)); return JS_UNDEFINED;
}
JSValue js_set_transform_origin(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    double x = 0.5, y = 0.5;
    if (argc < 2 || !numberValue(ctx, argv[0], x) || !numberValue(ctx, argv[1], y))
        return JS_ThrowTypeError(ctx, "setTransformOrigin requires x and y");
    target->setTransformOrigin(static_cast<float>(x), static_cast<float>(y)); return JS_UNDEFINED;
}

JSValue js_set_direction(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto name = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto value = name ? direction(*name) : std::nullopt;
    if (!value) return JS_ThrowRangeError(ctx, "unknown layout direction");
    target->setDirection(*value); return JS_UNDEFINED;
}

JSValue js_set_justify(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto name = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto value = name ? justify(*name) : std::nullopt;
    if (!value) return JS_ThrowRangeError(ctx, "unknown justify value");
    target->setJustifyContent(*value); return JS_UNDEFINED;
}

JSValue setAlign(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv,
                 bool selfAlignment) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto name = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto value = name ? align(*name) : std::nullopt;
    if (!value) return JS_ThrowRangeError(ctx, "unknown alignment value");
    if (selfAlignment) target->setAlignSelf(*value); else target->setAlignItems(*value);
    return JS_UNDEFINED;
}
JSValue js_set_align_items(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setAlign(c, s, a, v, false);
}
JSValue js_set_align_self(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return setAlign(c, s, a, v, true);
}

JSValue js_set_position_type(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto name = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!name) return JS_ThrowTypeError(ctx, "position type is required");
    if (*name == "static") target->setPositionType(layout::PositionType::Static);
    else if (*name == "relative") target->setPositionType(layout::PositionType::Relative);
    else if (*name == "absolute") target->setPositionType(layout::PositionType::Absolute);
    else return JS_ThrowRangeError(ctx, "position type must be static, relative, or absolute");
    return JS_UNDEFINED;
}

JSValue js_set_wrap(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto name = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    if (!name) return JS_ThrowTypeError(ctx, "wrap value is required");
    if (*name == "no-wrap" || *name == "nowrap") target->setWrap(layout::Wrap::NoWrap);
    else if (*name == "wrap") target->setWrap(layout::Wrap::Wrap);
    else if (*name == "wrap-reverse") target->setWrap(layout::Wrap::WrapReverse);
    else return JS_ThrowRangeError(ctx, "wrap must be no-wrap, wrap, or wrap-reverse");
    return JS_UNDEFINED;
}

template <typename Setter>
JSValue edgeSetter(JSContext* ctx, JSValueConst self, int argc,
                   JSValueConst* argv, const char* name, Setter setter) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    layout::Edge targetEdge = layout::Edge::All;
    int numberIndex = 0;
    if (argc >= 2) {
        const auto edgeName = stringValue(ctx, argv[0]);
        const auto parsed = edgeName ? edge(*edgeName) : std::nullopt;
        if (!parsed) return JS_ThrowRangeError(ctx, "%s received an unknown edge", name);
        targetEdge = *parsed;
        numberIndex = 1;
    }
    double value = 0.0;
    if (argc <= numberIndex || !numberValue(ctx, argv[numberIndex], value))
        return JS_ThrowTypeError(ctx, "%s requires [edge,] value", name);
    setter(*target, targetEdge, static_cast<float>(value));
    return JS_UNDEFINED;
}

JSValue js_set_padding(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return edgeSetter(c, s, a, v, "setPadding", [](auto& w, auto e, float n) { w.setPadding(e, n); });
}
JSValue js_set_margin(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    return edgeSetter(c, s, a, v, "setMargin", [](auto& w, auto e, float n) { w.setMargin(e, n); });
}
JSValue js_set_position(JSContext* c, JSValueConst s, int a, JSValueConst* v) {
    if (a < 2) return JS_ThrowTypeError(c, "setPosition requires edge and value");
    return edgeSetter(c, s, a, v, "setPosition", [](auto& w, auto e, float n) { w.setPosition(e, n); });
}

JSValue js_set_gap(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    layout::Gutter targetGutter = layout::Gutter::All;
    int numberIndex = 0;
    if (argc >= 2) {
        const auto name = stringValue(ctx, argv[0]);
        const auto value = name ? gutter(*name) : std::nullopt;
        if (!value) return JS_ThrowRangeError(ctx, "unknown gutter");
        targetGutter = *value;
        numberIndex = 1;
    }
    double value = 0.0;
    if (argc <= numberIndex || !numberValue(ctx, argv[numberIndex], value))
        return JS_ThrowTypeError(ctx, "setGap requires [gutter,] value");
    target->setGap(targetGutter, static_cast<float>(value));
    return JS_UNDEFINED;
}

JSValue js_set_visible(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "visible state is required");
    target->setVisible(JS_ToBool(ctx, argv[0]) != 0); return JS_UNDEFINED;
}
JSValue js_is_visible(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? JS_NewBool(ctx, target->isVisible()) : JS_EXCEPTION;
}
JSValue js_set_focusable(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "focusable state is required");
    target->setFocusable(JS_ToBool(ctx, argv[0]) != 0); return JS_UNDEFINED;
}
JSValue js_is_focusable(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? JS_NewBool(ctx, target->isFocusable()) : JS_EXCEPTION;
}
JSValue js_set_clips(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "clip state is required");
    target->setClipsToBounds(JS_ToBool(ctx, argv[0]) != 0); return JS_UNDEFINED;
}
JSValue js_get_opacity(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? JS_NewFloat64(ctx, target->getOpacity()) : JS_EXCEPTION;
}

JSValue js_remove_child(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* parent = widget(ctx, self); if (!parent) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "child widget is required");
    auto* child = widget(ctx, argv[0]); if (!child) return JS_EXCEPTION;
    if (child->getParent() != parent)
        return JS_ThrowTypeError(ctx, "widget is not a child of this parent");
    parent->removeChild(child); return JS_UNDEFINED;
}
JSValue js_add_child(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* parent = widget(ctx, self); if (!parent) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "child widget is required");
    auto child = detail::takeWidget(ctx, argv[0]); if (!child) return JS_EXCEPTION;
    parent->addChild(std::move(child)); return JS_UNDEFINED;
}

JSValue js_calculate_layout(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc == 0) target->calculateLayout();
    else {
        double width = 0.0, height = 0.0;
        if (argc < 2 || !numberValue(ctx, argv[0], width) || !numberValue(ctx, argv[1], height))
            return JS_ThrowTypeError(ctx, "calculateLayout requires width and height");
        target->calculateLayout(static_cast<float>(width), static_cast<float>(height));
    }
    return JS_UNDEFINED;
}

JSValue rectValue(JSContext* ctx, const lcl::graphics::RectF& rect) {
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "x", JS_NewFloat64(ctx, rect.x));
    JS_SetPropertyStr(ctx, result, "y", JS_NewFloat64(ctx, rect.y));
    JS_SetPropertyStr(ctx, result, "width", JS_NewFloat64(ctx, rect.width));
    JS_SetPropertyStr(ctx, result, "height", JS_NewFloat64(ctx, rect.height));
    return result;
}
JSValue js_get_bounds(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? rectValue(ctx, target->getBounds()) : JS_EXCEPTION;
}
JSValue js_get_absolute_bounds(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? rectValue(ctx, target->getAbsoluteBounds()) : JS_EXCEPTION;
}
JSValue js_get_layout_revision(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? JS_NewFloat64(ctx, static_cast<double>(target->getLayoutRevision())) : JS_EXCEPTION;
}
JSValue js_get_paint_revision(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? JS_NewFloat64(ctx, static_cast<double>(target->getPaintRevision())) : JS_EXCEPTION;
}
JSValue js_get_presentation_revision(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); return target ? JS_NewFloat64(ctx, static_cast<double>(target->getPresentationRevision())) : JS_EXCEPTION;
}

bool colorValue(JSContext* ctx, JSValueConst value, lcl::graphics::Color& color) {
    if (!JS_IsObject(value)) return false;
    std::array<int32_t, 4> channels{0, 0, 0, 255};
    const bool array = JS_IsArray(ctx, value);
    constexpr std::array<const char*, 4> names{"r", "g", "b", "a"};
    for (uint32_t index = 0; index < channels.size(); ++index) {
        JSValue channel = array ? JS_GetPropertyUint32(ctx, value, index)
                                : JS_GetPropertyStr(ctx, value, names[index]);
        if (index < 3 && JS_IsUndefined(channel)) { JS_FreeValue(ctx, channel); return false; }
        if (!JS_IsUndefined(channel) && JS_ToInt32(ctx, &channels[index], channel) < 0) {
            JS_FreeValue(ctx, channel); return false;
        }
        JS_FreeValue(ctx, channel);
    }
    color = {static_cast<uint8_t>(std::clamp(channels[0], 0, 255)),
             static_cast<uint8_t>(std::clamp(channels[1], 0, 255)),
             static_cast<uint8_t>(std::clamp(channels[2], 0, 255)),
             static_cast<uint8_t>(std::clamp(channels[3], 0, 255))};
    return true;
}

std::optional<double> numberProperty(JSContext* ctx, JSValueConst object, const char* name) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    double result = 0.0;
    const bool valid = !JS_IsUndefined(value) && !JS_IsNull(value) && numberValue(ctx, value, result);
    JS_FreeValue(ctx, value);
    return valid ? std::optional<double>(result) : std::nullopt;
}

void styleValues(JSContext* ctx, JSValueConst object, lcl::theme::StyleValues& output) {
    constexpr std::array<std::pair<const char*, std::optional<lcl::graphics::Color> lcl::theme::StyleValues::*>, 5> colors{{
        {"background", &lcl::theme::StyleValues::background},
        {"foreground", &lcl::theme::StyleValues::foreground},
        {"secondaryForeground", &lcl::theme::StyleValues::secondaryForeground},
        {"accent", &lcl::theme::StyleValues::accent},
        {"border", &lcl::theme::StyleValues::border},
    }};
    for (const auto& [name, member] : colors) {
        JSValue value = JS_GetPropertyStr(ctx, object, name);
        lcl::graphics::Color color;
        if (!JS_IsUndefined(value) && colorValue(ctx, value, color)) output.*member = color;
        JS_FreeValue(ctx, value);
    }
    if (const auto value = numberProperty(ctx, object, "borderWidth")) output.borderWidth = static_cast<float>(*value);
    if (const auto value = numberProperty(ctx, object, "cornerRadius")) output.cornerRadius = static_cast<float>(*value);
    if (const auto value = numberProperty(ctx, object, "scale")) output.scale = static_cast<float>(*value);
    if (const auto value = numberProperty(ctx, object, "opacity")) output.opacity = static_cast<float>(*value);
}

lcl::theme::WidgetStyle parseStyle(JSContext* ctx, JSValueConst object) {
    lcl::theme::WidgetStyle style;
    styleValues(ctx, object, style.normal);
    const std::array<std::pair<const char*, lcl::theme::StyleValues*>, 5> states{{
        {"normal", &style.normal}, {"hover", &style.hover}, {"pressed", &style.pressed},
        {"focused", &style.focused}, {"disabled", &style.disabled},
    }};
    for (const auto& [name, values] : states) {
        JSValue state = JS_GetPropertyStr(ctx, object, name);
        if (JS_IsObject(state)) styleValues(ctx, state, *values);
        JS_FreeValue(ctx, state);
    }
    if (const auto value = numberProperty(ctx, object, "horizontalPadding")) style.horizontalPadding = static_cast<float>(*value);
    if (const auto value = numberProperty(ctx, object, "verticalPadding")) style.verticalPadding = static_cast<float>(*value);
    return style;
}

std::optional<lcl::theme::WidgetStyleRole> styleRole(std::string_view value) {
    using Role = lcl::theme::WidgetStyleRole;
    if (value == "primarySurface" || value == "primary-surface") return Role::PrimarySurface;
    if (value == "secondarySurface" || value == "secondary-surface") return Role::SecondarySurface;
    if (value == "groupedSurface" || value == "grouped-surface") return Role::GroupedSurface;
    if (value == "primaryButton" || value == "primary-button") return Role::PrimaryButton;
    if (value == "quietButton" || value == "quiet-button") return Role::QuietButton;
    if (value == "textField" || value == "text-field") return Role::TextField;
    if (value == "toggle") return Role::Toggle;
    if (value == "picker") return Role::Picker;
    if (value == "slider") return Role::Slider;
    if (value == "progressView" || value == "progress-view") return Role::ProgressView;
    if (value == "tabView" || value == "tab-view") return Role::TabView;
    if (value == "menuItem" || value == "menu-item") return Role::MenuItem;
    if (value == "popover") return Role::Popover;
    return std::nullopt;
}

JSValue js_use_style(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "useStyle requires a style object");
    target->useStyle(parseStyle(ctx, argv[0])); return JS_UNDEFINED;
}
JSValue js_use_theme_style(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto name = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto role = name ? styleRole(*name) : std::nullopt;
    if (!role) return JS_ThrowRangeError(ctx, "unknown theme style role");
    target->useThemeStyle(*role); return JS_UNDEFINED;
}
JSValue js_clear_style(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    target->clearStyle(); return JS_UNDEFINED;
}

std::optional<lcl::ui::InteractionState> interactionState(std::string_view value) {
    using State = lcl::ui::InteractionState;
    if (value == "normal") return State::Normal;
    if (value == "hover") return State::Hover;
    if (value == "pressed" || value == "active") return State::Pressed;
    if (value == "focused" || value == "focus") return State::Focused;
    if (value == "disabled") return State::Disabled;
    return std::nullopt;
}
JSValue js_set_interaction_style(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "setInteractionStyle requires state and style");
    const auto name = stringValue(ctx, argv[0]);
    const auto state = name ? interactionState(*name) : std::nullopt;
    if (!state) return JS_ThrowRangeError(ctx, "unknown interaction state");
    lcl::ui::InteractionStyle style;
    if (const auto value = numberProperty(ctx, argv[1], "scale")) style.scale = static_cast<float>(*value);
    if (const auto value = numberProperty(ctx, argv[1], "opacity")) style.opacity = static_cast<float>(*value);
    JSValue motion = JS_GetPropertyStr(ctx, argv[1], "motion");
    if (JS_IsObject(motion)) {
        const auto duration = numberProperty(ctx, motion, "duration").value_or(180.0) / 1000.0;
        JSValue typeValue = JS_GetPropertyStr(ctx, motion, "type");
        const auto type = JS_IsUndefined(typeValue) ? std::optional<std::string>{"spring"} : stringValue(ctx, typeValue);
        JS_FreeValue(ctx, typeValue);
        style.motion = type && *type == "tween"
            ? lcl::motion::Motion::tween(static_cast<float>(duration), lcl::motion::Easing(lcl::motion::EasingName::EaseOutCubic))
            : lcl::motion::Motion::spring(static_cast<float>(duration),
                static_cast<float>(numberProperty(ctx, motion, "bounce").value_or(0.0)));
    }
    JS_FreeValue(ctx, motion);
    target->setInteractionStyle(*state, std::move(style)); return JS_UNDEFINED;
}
JSValue js_clear_interaction_style(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto name = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto state = name ? interactionState(*name) : std::nullopt;
    if (!state) return JS_ThrowRangeError(ctx, "unknown interaction state");
    target->clearInteractionStyle(*state); return JS_UNDEFINED;
}
JSValue js_set_interaction_enabled(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "enabled state is required");
    target->setInteractionEnabled(JS_ToBool(ctx, argv[0]) != 0); return JS_UNDEFINED;
}

JSValue js_set_on_click(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setOnClick requires a function or null");
    auto store = detail::setWidgetCallback(ctx, self, "click", argv[0]);
    if (!store && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) return JS_EXCEPTION;
    target->setOnClick(JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])
        ? std::function<void()>{}
        : std::function<void()>([store] { detail::invokeCallback(store, "click"); }));
    return JS_UNDEFINED;
}

JSValue js_set_effect(JSContext* ctx, JSValueConst self, int argc,
                      JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto sourceName = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto typeName = argc > 1 ? stringValue(ctx, argv[1]) : std::nullopt;
    const auto source = sourceName ? effectSource(*sourceName) : std::nullopt;
    const auto type = typeName ? effectType(*typeName) : std::nullopt;
    double value = 0.0, parameter1 = 0.0, parameter2 = 0.0;
    if (!source || !type || argc < 3 || !numberValue(ctx, argv[2], value) ||
        (argc > 3 && !numberValue(ctx, argv[3], parameter1)) ||
        (argc > 4 && !numberValue(ctx, argv[4], parameter2))) {
        return JS_ThrowTypeError(ctx,
            "setEffect requires source, type, value, and optional finite parameters");
    }
    lcl::ui::Effect effect{};
    effect.type = *type;
    effect.value = static_cast<float>(value);
    if (*type == lcl::ui::EffectType::Glass) {
        effect.value = 1.0f;
        effect.params[0] = static_cast<float>(std::max(0.0, value));
        effect.params[1] = static_cast<float>(std::max(0.0, parameter1));
        effect.params[2] = static_cast<float>(std::max(0.0, parameter2));
    }
    target->setEffects(*source, {effect});
    return JS_UNDEFINED;
}

JSValue js_clear_effects(JSContext* ctx, JSValueConst self, int argc,
                         JSValueConst* argv) {
    auto* target = widget(ctx, self); if (!target) return JS_EXCEPTION;
    const auto sourceName = argc > 0 ? stringValue(ctx, argv[0]) : std::nullopt;
    const auto source = sourceName ? effectSource(*sourceName) : std::nullopt;
    if (!source) return JS_ThrowRangeError(ctx, "unknown effect source");
    target->clearEffects(*source);
    return JS_UNDEFINED;
}

} // namespace

void registerWidgetExtensions(JSContext* ctx, JSValue prototype) {
    const std::tuple<const char*, JSCFunction*, int> methods[]{
        {"setWidth", js_set_width, 1}, {"setHeight", js_set_height, 1},
        {"setWidthAuto", js_set_width_auto, 0}, {"setHeightAuto", js_set_height_auto, 0},
        {"setMinWidth", js_set_min_width, 1}, {"setMinHeight", js_set_min_height, 1},
        {"setMaxWidth", js_set_max_width, 1}, {"setMaxHeight", js_set_max_height, 1},
        {"setDirection", js_set_direction, 1}, {"setJustifyContent", js_set_justify, 1},
        {"setAlignItems", js_set_align_items, 1}, {"setAlignSelf", js_set_align_self, 1},
        {"setPositionType", js_set_position_type, 1}, {"setWrap", js_set_wrap, 1},
        {"setFlexGrow", js_set_flex_grow, 1}, {"setFlexShrink", js_set_flex_shrink, 1},
        {"setFlexBasis", js_set_flex_basis, 1}, {"setFlexBasisAuto", js_set_flex_basis_auto, 0},
        {"setPadding", js_set_padding, 2}, {"setMargin", js_set_margin, 2},
        {"setGap", js_set_gap, 2}, {"setPosition", js_set_position, 2},
        {"setVisible", js_set_visible, 1}, {"isVisible", js_is_visible, 0},
        {"setFocusable", js_set_focusable, 1}, {"isFocusable", js_is_focusable, 0},
        {"setClipsToBounds", js_set_clips, 1}, {"getOpacity", js_get_opacity, 0},
        {"setOpacity", js_set_opacity, 1}, {"setTranslation", js_set_translation, 2},
        {"setScale", js_set_scale, 2}, {"setRotation", js_set_rotation, 1},
        {"setTransformOrigin", js_set_transform_origin, 2},
        {"setTranslationX", js_set_translation_x, 1}, {"setTranslationY", js_set_translation_y, 1},
        {"addChild", js_add_child, 1}, {"removeChild", js_remove_child, 1},
        {"calculateLayout", js_calculate_layout, 2},
        {"getBounds", js_get_bounds, 0}, {"getAbsoluteBounds", js_get_absolute_bounds, 0},
        {"getLayoutRevision", js_get_layout_revision, 0}, {"getPaintRevision", js_get_paint_revision, 0},
        {"getPresentationRevision", js_get_presentation_revision, 0},
        {"useStyle", js_use_style, 1}, {"useThemeStyle", js_use_theme_style, 1},
        {"clearStyle", js_clear_style, 0}, {"setOnClick", js_set_on_click, 1},
        {"setInteractionStyle", js_set_interaction_style, 2},
        {"clearInteractionStyle", js_clear_interaction_style, 1},
        {"setInteractionEnabled", js_set_interaction_enabled, 1},
        {"setEffect", js_set_effect, 5}, {"clearEffects", js_clear_effects, 1},
    };
    for (const auto& [name, function, argc] : methods)
        detail::addMethod(ctx, prototype, name, function, argc);
}

} // namespace lcl::binding
