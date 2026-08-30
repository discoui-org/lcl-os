#pragma once

namespace lcl::ui {

/**
 * Presentation-only caret blink state.
 *
 * Consumers own geometry, editing, and focus semantics; this controller only
 * exposes an opacity that can later evolve from hard phases to fades.
 */
class CaretPresentationController {
public:
    static constexpr float kVisiblePhaseDurationSec = 0.5f;
    static constexpr float kHiddenPhaseDurationSec = 0.5f;

    bool setActive(bool active) noexcept {
        if (!active) {
            const bool changed = m_opacity != 0.0f || m_active;
            m_active = false;
            m_phaseElapsedSec = 0.0f;
            m_visiblePhase = false;
            m_opacity = 0.0f;
            return changed;
        }
        if (m_active) return false;
        m_active = true;
        m_phaseElapsedSec = 0.0f;
        m_visiblePhase = true;
        m_opacity = 1.0f;
        return true;
    }

    bool resetActivity() noexcept {
        if (!m_active) return false;
        const bool changed = m_opacity != 1.0f;
        m_phaseElapsedSec = 0.0f;
        m_visiblePhase = true;
        m_opacity = 1.0f;
        return changed;
    }

    /** Advances hard-blink presentation time; returns true only on opacity change. */
    bool update(float deltaSec) noexcept {
        if (!m_active || deltaSec <= 0.0f) return false;

        m_phaseElapsedSec += deltaSec;
        const float previousOpacity = m_opacity;
        while (m_phaseElapsedSec >= currentPhaseDuration()) {
            m_phaseElapsedSec -= currentPhaseDuration();
            m_visiblePhase = !m_visiblePhase;
            m_opacity = m_visiblePhase ? 1.0f : 0.0f;
        }
        return m_opacity != previousOpacity;
    }

    float opacity() const noexcept { return m_opacity; }
    bool isActive() const noexcept { return m_active; }

private:
    float currentPhaseDuration() const noexcept {
        return m_visiblePhase ? kVisiblePhaseDurationSec : kHiddenPhaseDurationSec;
    }

    bool m_active{false};
    bool m_visiblePhase{false};
    float m_phaseElapsedSec{0.0f};
    float m_opacity{0.0f};
};

} // namespace lcl::ui
