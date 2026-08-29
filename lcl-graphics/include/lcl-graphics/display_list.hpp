#pragma once

#include <memory>
#include <optional>
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
struct BeginCachedLayerCommand {
    uint64_t id{0};
    RectF sourceBounds{};
    std::optional<RectF> updateBounds{};
};
struct EndCachedLayerCommand {};
struct DrawCachedLayerCommand {
    uint64_t id{0};
    RectF destination{};
    float opacity{1.0f};
    Matrix3 transform{};
};
struct DrawPathCommand { Path path{}; Paint paint{}; };
struct DrawTextCommand {
    PointF origin{};
    std::string text;
    Color color{};
    float fontSize{14.0f};
    FontFamily fontFamily{FontFamily::Interface};
};
struct DrawImageCommand {
    RectF destination{};
    uintptr_t resourceKey{0};
    uint64_t resourceId{0};
    uint64_t contentRevision{0};
    int sourceWidth{0};
    int sourceHeight{0};
    int stridePixels{0};
    bool opaque{false};
    float opacity{1.0f};
    float cornerRadius{0.0f};
    float cornerRoundness{2.0f};
    bool squareTopCorners{false};
};
enum class ExternalBufferSampleKind : uint8_t {
    Unresolved = 0,
    ArgbPixels = 1,
    GlTexture = 2,
};
/**
 * Stable external-node placeholder. Only nodeId and destination cross the
 * DisplayList wire; rasterd resolves the current granted buffer immediately
 * before replay and fills the runtime-only fields below.
 */
struct DrawExternalBufferCommand {
    uint64_t nodeId{0};
    RectF destination{};
    uintptr_t resourceKey{0};
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
    int sourceWidth{0};
    int sourceHeight{0};
    int stridePixels{0};
    ExternalBufferSampleKind sampleKind{ExternalBufferSampleKind::Unresolved};
};

using DisplayCommand = std::variant<SaveCommand, RestoreCommand, ConcatCommand,
                                    BeginLayerCommand, EndLayerCommand,
                                    ClipRectCommand, ClipPathCommand,
                                    ClearRectCommand,
                                    BeginCachedLayerCommand, EndCachedLayerCommand,
                                    DrawCachedLayerCommand,
                                    DrawPathCommand, DrawTextCommand,
                                    DrawImageCommand,
                                    DrawExternalBufferCommand>;

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
    void beginCachedLayerUpdate(uint64_t id, const RectF& sourceBounds,
                                const RectF& updateBounds);
    void endCachedLayer();
    void drawCachedLayer(uint64_t id, const RectF& destination, float opacity);
    void drawCachedLayerTransformed(uint64_t id, const RectF& destination,
                                    float opacity,
                                    const Matrix3& transform);
    void drawPath(const Path& path, const Paint& paint);
    void drawText(PointF origin, std::string text, Color color,
                  float fontSize,
                  FontFamily fontFamily = FontFamily::Interface);
    void drawImage(const RectF& destination, uintptr_t resourceKey,
                   int sourceWidth, int sourceHeight, int stridePixels,
                   float opacity,
                   float cornerRadius, float cornerRoundness,
                   bool squareTopCorners, uint64_t resourceId = 0,
                   uint64_t contentRevision = 0, bool opaque = false);
    void drawExternalBuffer(uint64_t nodeId, const RectF& destination);
    DisplayList build() const;
    void reset();

private:
    std::vector<DisplayCommand> m_commands;
};

} // namespace lcl::graphics
