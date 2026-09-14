#include "lcl-ui/core/motion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

void MotionCoordinator::setLayoutCallback(LayoutCallback layout) {
    m_layoutCallback = std::move(layout);
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
            m_engine.setValue(*channel, targetValue);
        }
        applyPresentation(targetValue);
        return;
    }
    // Morph renders the final presentation tree immediately and crossfades it
    // against a frozen pre-transaction raster snapshot. Animating an individual
    // style channel here would make the "new" side of that crossfade another
    // intermediate state instead of the committed final UI.
    if (m_transactionActive && m_options.layout == LayoutMode::Morph) {
        if (const auto channel = m_engine.findChannel(
                {widget.getObjectId(), static_cast<uint32_t>(property)})) {
            m_engine.setValue(*channel, targetValue);
        }
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
    float velocity = 0.0f;
    if (const auto existing = m_engine.findChannel(key)) {
        velocity = m_engine.sample(*existing).velocity;
    }
    if (!affectsLayout && m_compositorAnimationDelegate &&
        m_compositorAnimationDelegate(
            widget, property, presentationValue, targetValue, velocity, motion)) {
        if (const auto existing = m_engine.findChannel(key)) {
            (void)m_engine.stop(*existing, false);
            m_bindings.erase(*existing);
        }
        // The model/manifest advances to final state; compositor motion keeps
        // sampling the immutable old layer until it reaches that target.
        applyPresentation(targetValue);
        return;
    }
    const auto channel = m_engine.ensureChannel(key, presentationValue);
    if (!m_engine.isActive(channel)) m_engine.setValue(channel, presentationValue);
    m_bindings[channel] = Binding{widget.getObjectId(), &widget, std::move(applyPresentation), affectsLayout};
    m_engine.animateTo(channel, targetValue, motion);
}

bool MotionCoordinator::updateCompositorFloat(
        Widget& widget, AnimatableProperty property,
        float presentationValue, float targetValue,
        ApplyFloat applyPresentation) {
    if (!m_compositorAnimationDelegate || !applyPresentation) return false;
    constexpr float kInputSampleDurationSec = 1.0f / 240.0f;
    const auto motion = lcl::motion::Motion::tween(
        kInputSampleDurationSec, lcl::motion::Easing::linear());
    if (!m_compositorAnimationDelegate(
            widget, property, presentationValue, targetValue, 0.0f, motion)) {
        return false;
    }
    const lcl::motion::ChannelKey key{
        widget.getObjectId(), static_cast<uint32_t>(property)};
    if (const auto existing = m_engine.findChannel(key)) {
        (void)m_engine.stop(*existing, false);
        m_bindings.erase(*existing);
    }
    // Keep hit testing and a later settle declaration synchronized with the
    // most recent finger position. The compositor samples the visible layer;
    // the ordinary invalidation is coalesced and is not required for pacing.
    applyPresentation(targetValue);
    return true;
}

void MotionCoordinator::setColor(Widget& widget, AnimatableProperty firstChannel,
                                 graphics::Color presentation, graphics::Color target,
                                 std::function<void(graphics::Color)> applyPresentation,
                                 const lcl::motion::Motion* overrideMotion) {
    auto sharedColor = std::make_shared<graphics::Color>(presentation);
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
    const bool compositorProperty = property == AnimatableProperty::Opacity ||
        property == AnimatableProperty::TranslationX ||
        property == AnimatableProperty::TranslationY ||
        property == AnimatableProperty::ScaleX ||
        property == AnimatableProperty::ScaleY ||
        property == AnimatableProperty::Rotation;
    if (compositorProperty && m_compositorAnimationDelegate &&
        !keyframes.empty()) {
        const auto final = std::max_element(
            keyframes.begin(), keyframes.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.offset < rhs.offset;
            });
        const float start = widget.getPresentationValue(property);
        const auto motion = lcl::motion::Motion::tween(
            std::max(0.001f, options.durationSec), options.easing,
            std::max(0.0f, options.delaySec));
        if (m_compositorAnimationDelegate(
                widget, property, start, final->value, 0.0f, motion)) {
            widget.applyPresentationValue(property, final->value);
            return {};
        }
    }
    const auto lifetime = widget.getLifetimeToken();
    Widget* pointer = &widget;
    return m_timeline.animate(std::move(keyframes), options,
        [lifetime, pointer, property](float value) {
            if (lifetime.expired()) return;
            pointer->applyPresentationValue(property, value);
        },
        [lifetime, pointer, property] {
            return lifetime.expired() ? 0.0f : pointer->getPresentationValue(property);
        },
        [lifetime, pointer, property](float value) {
            if (!lifetime.expired()) pointer->commitModelValue(property, value);
        });
}

bool MotionCoordinator::tick(float dtSec, bool tickPresentationObservers) {
    const auto changed = m_engine.tick(dtSec);
    bool layoutChanged = false;
    for (const auto channel : changed) {
        const auto binding = m_bindings.find(channel);
        if (binding == m_bindings.end() || !binding->second.widget) continue;
        binding->second.apply(m_engine.sample(channel).value);
        layoutChanged = layoutChanged || binding->second.affectsLayout;
    }
    if (layoutChanged && m_layoutCallback) m_layoutCallback();
    m_timeline.tick(dtSec);
    if (tickPresentationObservers) tickPresentations(dtSec);
    return hasActiveAnimations();
}

bool MotionCoordinator::hasActiveAnimations() const noexcept {
    return m_engine.hasActiveAnimations() || m_timeline.hasActiveAnimations() ||
        !m_presentationBindings.empty() || !m_compositorAnimations.empty();
}

void MotionCoordinator::tickCompositorPresentations() {
    tickPresentations(0.0f);
}

bool MotionCoordinator::isObjectAnimating(uint64_t objectId) const {
    for (const auto& [channel, binding] : m_bindings) {
        if (binding.objectId == objectId && m_engine.isActive(channel)) return true;
    }
    const auto presentation = m_presentationBindings.find(objectId);
    return (presentation != m_presentationBindings.end() &&
            !presentation->second.lifetime.expired()) ||
        m_compositorAnimations.contains(objectId);
}

void MotionCoordinator::trackCompositorAnimation(
        uint64_t objectId, uint64_t transactionId) {
    if (objectId != 0 && transactionId != 0) {
        m_compositorAnimations[objectId] = transactionId;
    }
}

void MotionCoordinator::completeCompositorAnimation(
        uint64_t objectId, uint64_t transactionId) {
    const auto found = m_compositorAnimations.find(objectId);
    if (found != m_compositorAnimations.end() &&
        found->second == transactionId) {
        m_compositorAnimations.erase(found);
    }
}

void MotionCoordinator::registerPresentation(Widget& widget,
                                             PresentationCallback callback) {
    if (!callback) return;
    const uint64_t objectId = widget.getObjectId();
    m_presentationBindings[objectId] = PresentationBinding{
        widget.getLifetimeToken(), std::move(callback)};
}

void MotionCoordinator::unregisterPresentation(uint64_t objectId) {
    m_presentationBindings.erase(objectId);
}

void MotionCoordinator::tickPresentations(float dtSec) {
    // Snapshot IDs so callbacks may safely change registration without
    // invalidating this pass. The registry stays proportional to active
    // presentation owners, never the entire widget tree.
    std::vector<uint64_t> objectIds;
    objectIds.reserve(m_presentationBindings.size());
    for (const auto& [objectId, binding] : m_presentationBindings) {
        (void)binding;
        objectIds.push_back(objectId);
    }
    for (const uint64_t objectId : objectIds) {
        const auto it = m_presentationBindings.find(objectId);
        if (it == m_presentationBindings.end()) continue;
        if (it->second.lifetime.expired()) {
            m_presentationBindings.erase(it);
            continue;
        }
        PresentationCallback update = it->second.update;
        update(dtSec);
    }
}

void MotionCoordinator::unregisterObject(uint64_t objectId) {
    m_engine.clearObjectChannels(objectId);
    std::erase_if(m_bindings, [objectId](const auto& item) { return item.second.objectId == objectId; });
    unregisterPresentation(objectId);
    m_compositorAnimations.erase(objectId);
}

void MotionCoordinator::clear() {
    m_bindings.clear();
    m_presentationBindings.clear();
    m_compositorAnimations.clear();
    m_engine.clearAll();
    m_timeline.clear();
    m_transactionActive = false;
}

} // namespace lcl::ui
