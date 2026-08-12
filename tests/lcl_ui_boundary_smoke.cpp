#include <cstdint>
#include <memory>
#include <string>

#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"

namespace {

class BoundaryCanvas final : public lcl::ui::Canvas {
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

    void setContentScale(float) override {}
    void beginFrame() override {}
    void endFrame() override {}
    uint32_t* rasterBuffer() override { return m_pixels; }

    void drawRect(const lcl::ui::Rect&, lcl::ui::Color color) override {
        if (m_pixels && m_width > 0 && m_height > 0) {
            m_pixels[0] = (static_cast<uint32_t>(color.a) << 24) |
                          (static_cast<uint32_t>(color.r) << 16) |
                          (static_cast<uint32_t>(color.g) << 8) |
                          static_cast<uint32_t>(color.b);
        }
    }

    void drawRoundedRect(const lcl::ui::Rect& rect, float, lcl::ui::Color color,
                         lcl::ui::Color, float, float) override {
        drawRect(rect, color);
    }

    void drawTopRoundedRect(const lcl::ui::Rect& rect, float,
                            lcl::ui::Color color, float) override {
        drawRect(rect, color);
    }

    void drawText(float, float, const std::string&, lcl::ui::Color,
                  float, lcl::ui::FontFamily) override {}

    float measureText(const std::string&, float, lcl::ui::FontFamily) override {
        return 0.0f;
    }

    void drawBuffer(int, int, int, int, const uint32_t*, int, float,
                    float, float, bool, int, int) override {}

private:
    uint32_t* m_pixels{nullptr};
    uint32_t m_width{0};
    uint32_t m_height{0};
};

} // namespace

int main() {
    lcl::ui::WindowApp app(std::make_unique<BoundaryCanvas>(), 8, 8,
                           "lcl-ui boundary smoke");
    auto root = std::make_unique<lcl::ui::Container>();
    root->setBackgroundColor({12, 34, 56, 255});
    root->getYogaNode().setWidth(8.0f);
    root->getYogaNode().setHeight(8.0f);
    app.setRootWidget(std::move(root));

    if (!app.renderFrame()) return 1;
    return app.getPixelBuffer()[0] == 0xFF0C2238u ? 0 : 2;
}
