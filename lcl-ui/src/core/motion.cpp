#include "lcl-ui/core/motion.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

void MotionCoordinator::setCallbacks(LayoutCallback layout, DamageCallback damage) {
    m_layoutCallback = std::move(layout);
    m_damageCallback = std::move(damage);
}

void MotionCoordinator::beginTransaction(const lcl::motion::Motion& motion,
                                         AnimationTransactionOptions options) {
    m_motion = motion;
    m_options = options;
    m_transactionActive = true;
}

void MotionCoordinator::endTransaction() { m_transactionActive = false; }

void MotionCoordinator::setFloat(Widget& widget, AnimatableProperty property,
                                 float presentationValue, float targetValue,
                                 ApplyFloat applyPresentation, bool affectsLayout,
                                 const lcl::motion::Motion* overrideMotion) {
    if (!m_transactionActive && !overrideMotion) {
        if (const auto channel = m_engine.findChannel(
                {widget.getObjectId(), static_cast<uint32_t>(property)})) {
            m_engine.stop(*channel, false);
        }
        applyPresentation(targetValue);
        return;
    }
    if (m_transactionActive && m_options.layout == LayoutMode::Morph && affectsLayout) {
        applyPresentation(targetValue);
        return;
    }
    animateFloat(widget, property, presentationValue, targetValue,
                 overrideMotion ? *overrideMotion : m_motion,
                 std::move(applyPresentation), affectsLayout);
}

void MotionCoordinator::animateFloat(Widget& widget, AnimatableProperty property,
                                     float presentationValue, float targetValue,
                                     const lcl::motion::Motion& motion,
                                     ApplyFloat applyPresentation, bool affectsLayout) {
    const lcl::motion::ChannelKey key{widget.getObjectId(), static_cast<uint32_t>(property)};
    const auto channel = m_engine.ensureChannel(key, presentationValue);
    m_bindings[channel] = Binding{widget.getObjectId(), &widget, std::move(applyPresentation), affectsLayout};
    m_engine.animateTo(channel, targetValue, motion);
}

void MotionCoordinator::setColor(Widget& widget, AnimatableProperty firstChannel,
                                 Color presentation, Color target,
                                 std::function<void(Color)> applyPresentation,
                                 const lcl::motion::Motion* overrideMotion) {
    auto sharedColor = std::make_shared<Color>(presentation);
    const std::array<float, 4> current{static_cast<float>(presentation.r), static_cast<float>(presentation.g),
                                       static_cast<float>(presentation.b), static_cast<float>(presentation.a)};
    const std::array<float, 4> destination{static_cast<float>(target.r), static_cast<float>(target.g),
                                           static_cast<float>(target.b), static_cast<float>(target.a)};
    for (uint32_t component = 0; component < 4; ++component) {
        const auto property = static_cast<AnimatableProperty>(static_cast<uint32_t>(firstChannel) + component);
        setFloat(widget, property, current[component], destination[component],
            [sharedColor, applyPresentation, component](float value) {
                const uint8_t channel = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
                switch (component) {
                    case 0: sharedColor->r = channel; break;
                    case 1: sharedColor->g = channel; break;
                    case 2: sharedColor->b = channel; break;
                    default: sharedColor->a = channel; break;
                }
                applyPresentation(*sharedColor);
            }, false, overrideMotion);
    }
}

lcl::motion::AnimationHandle MotionCoordinator::animate(
        Widget& widget, AnimatableProperty property,
        std::vector<lcl::motion::Keyframe> keyframes,
        const lcl::motion::AnimationOptions& options) {
    const auto lifetime = widget.getLifetimeToken();
    Widget* pointer = &widget;
    return m_timeline.animate(std::move(keyframes), options,
        [lifetime, pointer, property, damage = m_damageCallback](float value) {
            if (lifetime.expired()) return;
            const Rect before = pointer->getPresentationBounds();
            pointer->applyPresentationValue(property, value);
            if (damage) {
                damage(before);
                damage(pointer->getPresentationBounds());
            }
        },
        [lifetime, pointer, property] {
            return lifetime.expired() ? 0.0f : pointer->getPresentationValue(property);
        },
        [lifetime, pointer, property](float value) {
            if (!lifetime.expired()) pointer->commitModelValue(property, value);
        });
}

bool MotionCoordinator::tick(float dtSec) {
    const auto changed = m_engine.tick(dtSec);
    bool layoutChanged = false;
    for (const auto channel : changed) {
        const auto binding = m_bindings.find(channel);
        if (binding == m_bindings.end() || !binding->second.widget) continue;
        const Rect oldBounds = binding->second.widget->getPresentationBounds();
        binding->second.apply(m_engine.sample(channel).value);
        layoutChanged = layoutChanged || binding->second.affectsLayout;
        if (m_damageCallback) m_damageCallback(oldBounds);
    }
    if (layoutChanged && m_layoutCallback) m_layoutCallback();
    for (const auto channel : changed) {
        const auto binding = m_bindings.find(channel);
        if (binding != m_bindings.end() && binding->second.widget && m_damageCallback)
            m_damageCallback(binding->second.widget->getPresentationBounds());
    }
    m_timeline.tick(dtSec);
    return hasActiveAnimations();
}

bool MotionCoordinator::hasActiveAnimations() const noexcept {
    return m_engine.hasActiveAnimations() || m_timeline.hasActiveAnimations();
}

bool MotionCoordinator::isObjectAnimating(uint64_t objectId) const {
    for (const auto& [channel, binding] : m_bindings) {
        if (binding.objectId == objectId && m_engine.isActive(channel)) return true;
    }
    return false;
}

void MotionCoordinator::unregisterObject(uint64_t objectId) {
    m_engine.clearObjectChannels(objectId);
    std::erase_if(m_bindings, [objectId](const auto& item) { return item.second.objectId == objectId; });
}

void MotionCoordinator::clear() {
    m_bindings.clear();
    m_engine.clearAll();
    m_timeline.clear();
    m_transactionActive = false;
}

} // namespace lcl::ui
