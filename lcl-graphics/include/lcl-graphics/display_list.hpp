#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "lcl-graphics/path.hpp"
#include "lcl-graphics/font.hpp"

namespace lcl::graphics {

struct SaveCommand {};
struct RestoreCommand {};
struct ConcatCommand { Matrix3 transform{}; };
struct BeginLayerCommand { float opacity{1.0f}; };
struct EndLayerCommand {};
struct ClipRectCommand { RectF rect{}; };
struct ClipPathCommand { Path path{}; FillRule fillRule{FillRule::NonZero}; };
struct ClearRectCommand { RectF rect{}; Color color{}; };
struct BeginCachedLayerCommand { uint64_t id{0}; RectF sourceBounds{}; };
struct EndCachedLayerCommand {};
struct DrawCachedLayerCommand {
    uint64_t id{0};
    RectF destination{};
    float opacity{1.0f};
};
struct DrawPathCommand { Path path{}; Paint paint{}; };
struct DrawTextCommand {
    PointF origin{};
    std::string text;
    Color color{};
    float fontSize{14.0f};
    FontFamily fontFamily{FontFamily::Interface};
    bool rasterized{false};
};
struct DrawImageCommand {
    RectF destination{};
    uintptr_t resourceKey{0};
    int sourceWidth{0};
    int sourceHeight{0};
    int stridePixels{0};
    float opacity{1.0f};
    float cornerRadius{0.0f};
    float cornerRoundness{2.0f};
    bool squareTopCorners{false};
};

using DisplayCommand = std::variant<SaveCommand, RestoreCommand, ConcatCommand,
                                    BeginLayerCommand, EndLayerCommand,
                                    ClipRectCommand, ClipPathCommand,
                                    ClearRectCommand,
                                    BeginCachedLayerCommand, EndCachedLayerCommand,
                                    DrawCachedLayerCommand,
                                    DrawPathCommand, DrawTextCommand,
                                    DrawImageCommand>;

class DisplayList {
public:
    DisplayList() = default;
    explicit DisplayList(std::shared_ptr<const std::vector<DisplayCommand>> commands)
        : m_commands(std::move(commands)) {}

    const std::vector<DisplayCommand>& commands() const noexcept;
    bool empty() const noexcept { return commands().empty(); }

private:
    std::shared_ptr<const std::vector<DisplayCommand>> m_commands;
};

class DisplayListBuilder {
public:
    void save();
    void restore();
    void concat(const Matrix3& transform);
    void beginLayer(float opacity);
    void endLayer();
    void clipRect(const RectF& rect);
    void clipPath(const Path& path, FillRule fillRule = FillRule::NonZero);
    void clearRect(const RectF& rect, Color color);
    void beginCachedLayer(uint64_t id, const RectF& sourceBounds);
    void endCachedLayer();
    void drawCachedLayer(uint64_t id, const RectF& destination, float opacity);
    void drawPath(const Path& path, const Paint& paint);
    void drawText(PointF origin, std::string text, Color color,
                  float fontSize, FontFamily fontFamily = FontFamily::Interface,
                  bool rasterized = false);
    void drawImage(const RectF& destination, uintptr_t resourceKey,
                   int sourceWidth, int sourceHeight, int stridePixels,
                   float opacity,
                   float cornerRadius, float cornerRoundness,
                   bool squareTopCorners);
    DisplayList build() const;
    void reset();

private:
    std::vector<DisplayCommand> m_commands;
};

} // namespace lcl::graphics
