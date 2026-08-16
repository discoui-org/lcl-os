#pragma once

#include <optional>
#include <string>

#include "lcl-ui/core/image_loader.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

enum class ImageFit {
    Fill,
    Contain,
    Cover
};

class Image : public Widget {
public:
    explicit Image(const std::string& sourcePath = "");
    ~Image() override = default;

    bool setSourcePath(const std::string& sourcePath);
    const std::string& getSourcePath() const { return m_sourcePath; }

    void clearSource();
    bool hasImage() const { return m_sourceImage.has_value(); }

    void setFit(ImageFit fit) { m_fit = fit; markDirty(); }
    ImageFit getFit() const { return m_fit; }

    void setCornerRadius(float radiusPx);
    float getCornerRadius() const { return m_cornerRadius; }

    void setCornerRoundness(float roundness);
    float getCornerRoundness() const { return m_cornerRoundness; }

    void setOpacity(float opacity);
    float getOpacity() const { return m_opacity; }

    void draw(Canvas& canvas, const Rect& damageRect) override;

private:
    std::string m_sourcePath;
    std::optional<ImageData> m_sourceImage;
    ImageFit m_fit{ImageFit::Contain};
    float m_cornerRadius{0.0f};
    float m_cornerRoundness{2.0f};
    float m_opacity{1.0f};
};

} // namespace lcl::ui
