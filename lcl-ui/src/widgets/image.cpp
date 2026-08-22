#include "lcl-ui/widgets/image.hpp"

#include <algorithm>
#include <cmath>

#include "lcl-graphics/canvas.hpp"

namespace lcl::ui {

Image::Image(const std::string& sourcePath) {
    if (!sourcePath.empty()) {
        setSourcePath(sourcePath);
    }
}

bool Image::setSourcePath(const std::string& sourcePath) {
    if (sourcePath == m_sourcePath && m_sourceImage.has_value()) {
        return true;
    }

    auto loaded = ImageLoader::loadArgb32(sourcePath);
    if (!loaded || !loaded->isValid()) {
        m_sourcePath.clear();
        m_sourceImage.reset();
        markDirty();
        return false;
    }

    m_sourcePath = sourcePath;
    m_sourceImage = std::move(loaded);
    markDirty();
    return true;
}

void Image::clearSource() {
    if (m_sourcePath.empty() && !m_sourceImage.has_value()) {
        return;
    }

    m_sourcePath.clear();
    m_sourceImage.reset();
    markDirty();
}

void Image::setCornerRadius(float radius) {
    float clamped = std::max(0.0f, radius);
    if (std::fabs(clamped - m_cornerRadius) <= 0.001f) {
        return;
    }
    m_cornerRadius = clamped;
    markDirty();
}

void Image::setCornerRoundness(float roundness) {
    float clamped = std::clamp(roundness, 2.0f, 8.0f);
    if (std::fabs(clamped - m_cornerRoundness) <= 0.001f) {
        return;
    }
    m_cornerRoundness = clamped;
    markDirty();
}

void Image::setOpacity(float opacity) {
    float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (std::fabs(clamped - m_opacity) <= 0.001f) {
        return;
    }
    m_opacity = clamped;
    markDirty();
}

void Image::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect) || !m_sourceImage || !m_sourceImage->isValid()) {
        return;
    }

    beginPresentation(canvas);

    const float boxX = m_absoluteBounds.x;
    const float boxY = m_absoluteBounds.y;
    const float boxW = std::max(0.0f, m_absoluteBounds.width);
    const float boxH = std::max(0.0f, m_absoluteBounds.height);
    if (boxW <= 0 || boxH <= 0) {
        endPresentation(canvas);
        return;
    }

    const int srcW = static_cast<int>(m_sourceImage->width);
    const int srcH = static_cast<int>(m_sourceImage->height);
    if (srcW <= 0 || srcH <= 0) {
        endPresentation(canvas);
        return;
    }

    float drawW = boxW;
    float drawH = boxH;
    float drawX = boxX;
    float drawY = boxY;

    if (m_fit == ImageFit::Contain) {
        const float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
        const float boxAspect = static_cast<float>(boxW) / static_cast<float>(boxH);

        if (srcAspect > boxAspect) {
            drawW = boxW;
            drawH = std::max(1.0f, drawW / srcAspect);
        } else {
            drawH = boxH;
            drawW = std::max(1.0f, drawH * srcAspect);
        }

        drawX = boxX + (boxW - drawW) / 2;
        drawY = boxY + (boxH - drawH) / 2;
    } else if (m_fit == ImageFit::Cover) {
        const float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
        const float boxAspect = static_cast<float>(boxW) / static_cast<float>(boxH);

        if (srcAspect > boxAspect) {
            drawH = boxH;
            drawW = std::max(1.0f, drawH * srcAspect);
        } else {
            drawW = boxW;
            drawH = std::max(1.0f, drawW / srcAspect);
        }

        drawX = boxX + (boxW - drawW) / 2;
        drawY = boxY + (boxH - drawH) / 2;
    }

    canvas.drawBuffer({drawX, drawY, drawW, drawH}, srcW, srcH,
                      m_sourceImage->pixels.data(), srcW, m_opacity,
                      m_cornerRadius, m_cornerRoundness, false);

    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

} // namespace lcl::ui
