#include "lcl-ui/widgets/progress_view.hpp"

#include "lcl-graphics/canvas.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::ui {
namespace {

constexpr float kLinearWidth = 160.0f;
constexpr float kLinearHeight = 16.0f;
constexpr float kCircularSize = 22.0f;
constexpr float kTau = 6.28318530718f;

graphics::PointF pointOnCircle(float centerX, float centerY, float radius,
                               float angle) {
    return {centerX + std::cos(angle) * radius,
            centerY + std::sin(angle) * radius};
}

} // namespace

ProgressView::ProgressView() {
    setDefaultWidth(kCircularSize);
    setDefaultHeight(kCircularSize);
}

ProgressView::ProgressView(float value, float total)
    : m_value(value), m_total(std::max(0.0001f, total)) {
    m_value = std::clamp(*m_value, 0.0f, m_total);
    setDefaultWidth(kLinearWidth);
    setDefaultHeight(kLinearHeight);
}

void ProgressView::setValue(std::optional<float> value) {
    if (value) value = std::clamp(*value, 0.0f, m_total);
    if (m_value == value) return;
    m_value = value;
    if (m_style == ProgressViewStyle::Automatic) {
        if (!m_hasWidth) setDefaultWidth(
            m_value ? kLinearWidth : kCircularSize);
        if (!m_hasHeight) setDefaultHeight(
            m_value ? kLinearHeight : kCircularSize);
    }
    updatePresentationRegistration();
    markDirty();
}

void ProgressView::setTotal(float total) {
    if (!std::isfinite(total) || total <= 0.0f) return;
    if (std::fabs(total - m_total) <= 0.0001f) return;
    m_total = total;
    if (m_value) m_value = std::clamp(*m_value, 0.0f, m_total);
    markDirty();
}

void ProgressView::setProgressViewStyle(ProgressViewStyle style) {
    if (m_style == style) return;
    m_style = style;
    const bool circular = resolvedProgressViewStyle() ==
        ProgressViewStyle::Circular;
    if (!m_hasWidth) setDefaultWidth(circular ? kCircularSize : kLinearWidth);
    if (!m_hasHeight) setDefaultHeight(circular ? kCircularSize : kLinearHeight);
    markDirty();
}

ProgressViewStyle ProgressView::resolvedProgressViewStyle() const noexcept {
    return m_style == ProgressViewStyle::Automatic
        ? (m_value ? ProgressViewStyle::Linear : ProgressViewStyle::Circular)
        : m_style;
}

float ProgressView::normalizedValue() const noexcept {
    return m_value ? std::clamp(*m_value / m_total, 0.0f, 1.0f) : 0.0f;
}

void ProgressView::updatePresentationRegistration() {
    if (m_value) {
        if (m_motionCoordinator) {
            m_motionCoordinator->unregisterPresentation(getObjectId());
        }
        return;
    }
    if (!m_motionCoordinator) return;
    // Registration is idempotent. Re-registering during draw also makes a
    // ProgressView safe when it moves between WindowApp coordinators.
    m_motionCoordinator->registerPresentation(
        *this, [this](float deltaSec) { tickIndeterminate(deltaSec); });
}

void ProgressView::tickIndeterminate(float deltaSec) {
    if (m_value) return;
    m_phase = std::fmod(m_phase + std::max(0.0f, deltaSec) * 0.85f, 1.0f);
    markPresentationDirty();
}

const lcl::theme::WidgetStyle* ProgressView::defaultStyle() const noexcept {
    return &getTheme().progressView;
}

void ProgressView::styleDidChange() {
    markDirty();
}

void ProgressView::drawLinear(
        graphics::Canvas& canvas,
        const lcl::theme::ResolvedStyle& visual) {
    const float height = std::min(getTheme().metrics.progressTrackHeight,
                                  m_absoluteBounds.height);
    const graphics::RectF track{
        m_absoluteBounds.x,
        m_absoluteBounds.y + (m_absoluteBounds.height - height) * 0.5f,
        m_absoluteBounds.width, height};
    canvas.drawRoundedRect(track, height * 0.5f, visual.background,
                           {}, 0.0f, 1.0f);
    if (m_value) {
        const graphics::RectF fill{track.x, track.y,
                                   track.width * normalizedValue(), height};
        if (fill.width > 0.0f) {
            canvas.drawRoundedRect(fill, height * 0.5f, visual.accent,
                                   {}, 0.0f, 1.0f);
        }
        return;
    }

    canvas.saveState();
    canvas.clipRect(track);
    const float segmentWidth = track.width * 0.32f;
    const float travel = track.width + segmentWidth;
    const float x = track.x - segmentWidth + travel * m_phase;
    canvas.drawRoundedRect({x, track.y, segmentWidth, height},
                           height * 0.5f, visual.accent, {}, 0.0f, 1.0f);
    canvas.restoreState();
}

void ProgressView::drawCircular(
        graphics::Canvas& canvas,
        const lcl::theme::ResolvedStyle& visual) {
    const float size = std::min(m_absoluteBounds.width, m_absoluteBounds.height);
    const float strokeWidth = std::max(2.0f,
        std::min(getTheme().metrics.progressTrackHeight, size * 0.2f));
    const graphics::RectF ring{
        m_absoluteBounds.x + (m_absoluteBounds.width - size) * 0.5f + strokeWidth,
        m_absoluteBounds.y + (m_absoluteBounds.height - size) * 0.5f + strokeWidth,
        std::max(0.0f, size - strokeWidth * 2.0f),
        std::max(0.0f, size - strokeWidth * 2.0f)};
    graphics::Paint trackPaint;
    trackPaint.color = visual.background;
    trackPaint.style = graphics::PaintStyle::Stroke;
    trackPaint.stroke.width = strokeWidth;
    trackPaint.stroke.cap = graphics::StrokeCap::Round;
    canvas.drawEllipse(ring, trackPaint);

    const float progress = m_value ? normalizedValue() : 0.72f;
    if (progress <= 0.0f) return;
    graphics::Paint fillPaint = trackPaint;
    fillPaint.color = visual.accent;
    if (progress >= 0.999f) {
        canvas.drawEllipse(ring, fillPaint);
        return;
    }

    const float centerX = ring.x + ring.width * 0.5f;
    const float centerY = ring.y + ring.height * 0.5f;
    const float radius = ring.width * 0.5f;
    const float start = -kTau * 0.25f + (m_value ? 0.0f : m_phase * kTau);
    const float end = start + progress * kTau;
    const auto startPoint = pointOnCircle(centerX, centerY, radius, start);
    const auto endPoint = pointOnCircle(centerX, centerY, radius, end);
    graphics::Path arc;
    arc.moveTo(startPoint.x, startPoint.y)
       .arcTo(radius, radius, 0.0f, progress > 0.5f, true,
              endPoint.x, endPoint.y);
    canvas.drawPath(arc, fillPaint);
}

void ProgressView::draw(graphics::Canvas& canvas,
                        const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationPaintBounds().intersects(damageRect)) return;
    updatePresentationRegistration();
    beginPresentation(canvas);
    const auto visual = lcl::theme::resolveStyle(
        *resolvedStyle(), lcl::theme::StyleState::Normal);
    if (resolvedProgressViewStyle() == ProgressViewStyle::Circular) {
        drawCircular(canvas, visual);
    } else {
        drawLinear(canvas, visual);
    }
    endPresentation(canvas);
}

} // namespace lcl::ui
