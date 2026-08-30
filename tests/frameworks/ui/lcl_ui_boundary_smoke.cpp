#include <cstdint>
#include <memory>
#include <string>

#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"

namespace {

class BoundaryCanvas final : public lcl::graphics::Canvas {
public:
    bool initialize(uint32_t width, uint32_t height, uint32_t* pixels) override {
        setTargetPixels(pixels, width, height);
        return m_pixels != nullptr;
    }

    void setTargetPixels(uint32_t* pixels, uint32_t width, uint32_t height) override {
        m_pixels = pixels;
        m_width = width;
        m_height = height;
    }

    void setRenderTarget(const lcl::graphics::RenderTarget& target) override {
        m_target = target;
    }
    const lcl::graphics::RenderTarget& renderTarget() const override { return m_target; }
    void beginFrame() override {}
    void endFrame() override {}
    uint32_t* rasterBuffer() override { return m_pixels; }

    void clearRect(const lcl::graphics::RectF& rect, lcl::graphics::Color color) override {
        drawRect(rect, color);
    }

    void drawPath(const lcl::graphics::Path&, const lcl::graphics::Paint&) override {}

    void drawRect(const lcl::graphics::RectF&, lcl::graphics::Color color) override {
        if (m_pixels && m_width > 0 && m_height > 0) {
            m_pixels[0] = (static_cast<uint32_t>(color.a) << 24) |
                          (static_cast<uint32_t>(color.r) << 16) |
                          (static_cast<uint32_t>(color.g) << 8) |
                          static_cast<uint32_t>(color.b);
        }
    }

    void drawRoundedRect(const lcl::graphics::RectF& rect, float, lcl::graphics::Color color,
                         lcl::graphics::Color, float, float) override {
        drawRect(rect, color);
    }

    void drawTopRoundedRect(const lcl::graphics::RectF& rect, float,
                            lcl::graphics::Color color, float) override {
        drawRect(rect, color);
    }

    void drawText(float, float, const std::string&, lcl::graphics::Color,
                  float, lcl::graphics::FontFamily) override {}

    float measureText(const std::string&, float, lcl::graphics::FontFamily) override {
        return 0.0f;
    }

    void drawBuffer(const lcl::graphics::RectF&, int, int, const uint32_t*, int,
                    float, float, float, bool) override {}

private:
    uint32_t* m_pixels{nullptr};
    uint32_t m_width{0};
    uint32_t m_height{0};
    lcl::graphics::RenderTarget m_target{};
};

} // namespace

int main() {
    lcl::ui::WindowApp app(std::make_unique<BoundaryCanvas>(), 8, 8,
                           "lcl-ui boundary smoke");
    auto root = std::make_unique<lcl::ui::Container>();
    root->setBackgroundColor({12, 34, 56, 255});
    root->setWidth(8.0f);
    root->setHeight(8.0f);
    root->setDirection(lcl::ui::layout::Direction::Column);
    root->setAlignItems(lcl::ui::layout::Align::Stretch);
    root->setPadding(0.0f);
    root->setMargin(0.0f);
    root->setGap(0.0f);
    app.setRootWidget(std::move(root));

    if (!app.renderFrame()) return 1;
    return app.getPixelBuffer()[0] == 0xFF0C2238u ? 0 : 2;
}
