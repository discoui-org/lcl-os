#include <gtest/gtest.h>
#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/core/caret_presentation_controller.hpp"
#include "lcl-graphics/geometry.hpp"
#include "lcl-graphics/display_list_wire.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/measured_widget.hpp"
#include "lcl-ui/widgets/widget.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/icon.hpp"
#include "lcl-ui/widgets/navigation_split_view.hpp"
#include "lcl-ui/widgets/navigation_stack.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/menu.hpp"
#include "lcl-ui/widgets/popover.hpp"
#include "lcl-ui/widgets/window_chrome.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/text_field.hpp"
#include "lcl-ui/widgets/toggle.hpp"
#include "lcl-ui/widgets/slider.hpp"
#include "lcl-ui/widgets/progress_view.hpp"
#include "system/render/raster_renderer.hpp"
#include "system/render/raster_canvas.hpp"
#include "system/render/alpha_math.hpp"
#include "system/render/path_rasterizer.hpp"
#include "system/render/backdrop_filter_geometry.hpp"
#include "system/render/text_metrics.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace lcl::ui;
namespace graphics = lcl::graphics;

namespace {

class RecordingCanvas final : public graphics::Canvas {
public:
    bool initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) override {
        initialized = true;
        setTargetPixels(targetPixels, width, height);
        return true;
    }

    void setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) override {
        ++targetSetCount;
        pixels = targetPixels;
        pixelWidth = width;
        pixelHeight = height;
    }

    void setRenderTarget(const graphics::RenderTarget& value) override {
        target = value;
        deviceScale = value.deviceScale;
    }
    const graphics::RenderTarget& renderTarget() const override { return target; }
    void beginFrame() override { ++beginCount; }
    void endFrame() override { ++endCount; }
    uint32_t* rasterBuffer() override { return pixels; }
    void setDmaBufTransportEnabled(bool enabled) override { dmaBufEnabled = enabled; }
    bool hasDmaBufTransport() const override { return dmaBufAvailable && dmaBufEnabled; }
    bool configureDmaBufFrame(uint32_t contentWidth, uint32_t contentHeight,
                              uint32_t backingWidth, uint32_t backingHeight) override {
        if (!hasDmaBufTransport()) return false;
        if (backingWidth > dmaCapacityWidth || backingHeight > dmaCapacityHeight) {
            dmaCapacityWidth = std::max(dmaCapacityWidth, backingWidth);
            dmaCapacityHeight = std::max(dmaCapacityHeight, backingHeight);
            ++dmaCapacityGrowCount;
        }
        dmaContentWidth = contentWidth;
        dmaContentHeight = contentHeight;
        dmaBackingWidth = backingWidth;
        dmaBackingHeight = backingHeight;
        ++dmaConfigureCount;
        return true;
    }
    bool isDmaBufFrameActive() const override { return hasDmaBufTransport(); }
    std::optional<graphics::DmaBufFrame> takeDmaBufFrame() override {
        if (!hasDmaBufTransport()) return std::nullopt;
        const int fd = dup(STDIN_FILENO);
        if (fd < 0) return std::nullopt;
        return graphics::DmaBufFrame{nextBufferId++, dmaContentWidth, dmaContentHeight,
                           dmaBackingWidth, dmaBackingHeight, dmaBackingWidth * 4,
                           1u, ~uint64_t{0}, fd};
    }
    void releaseDmaBufFrame(uint32_t bufferId) override { releasedBufferIds.push_back(bufferId); }
    void clipRect(const graphics::RectF& rect) override { clips.push_back(rect); }
    bool beginCachedLayer(CachedLayerId id, const graphics::RectF& bounds) override {
        ++cachedLayerBeginCount;
        activeCachedLayer = id;
        cachedLayerBounds.push_back(bounds);
        return true;
    }
    bool beginCachedLayerUpdate(CachedLayerId id,
                                const graphics::RectF& bounds,
                                const graphics::RectF& updateBounds) override {
        ++cachedLayerUpdateBeginCount;
        activeCachedLayer = id;
        cachedLayerBounds.push_back(bounds);
        cachedLayerUpdateBounds.push_back(updateBounds);
        return cachedLayers.contains(id);
    }
    void endCachedLayer() override {
        ++cachedLayerEndCount;
        cachedLayers.insert(activeCachedLayer);
        activeCachedLayer = 0;
    }
    bool drawCachedLayer(CachedLayerId id, const graphics::RectF& destination,
                         float) override {
        if (!cachedLayers.contains(id)) return false;
        ++cachedLayerDrawCount;
        cachedLayerDestinations.push_back(destination);
        return true;
    }
    bool drawCachedLayerTransformed(
            CachedLayerId id, const graphics::RectF& destination,
            const graphics::Matrix3& transform, float opacity) override {
        cachedLayerTransforms.push_back(transform);
        return drawCachedLayer(id, destination, opacity);
    }

    void clearRect(const graphics::RectF& rect, graphics::Color color) override {
        clearedRects.push_back(rect);
        clearColors.push_back(color);
    }

    void drawPath(const graphics::Path&, const graphics::Paint&) override {
        ++pathDrawCount;
    }

    void drawRect(const graphics::RectF& rect, graphics::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
    }

    void drawRoundedRect(const graphics::RectF& rect, float radius, graphics::Color color,
                         graphics::Color border, float borderWidth, float roundness) override {
        roundedRects.push_back(rect);
        roundedRadii.push_back(radius);
        colors.push_back(color);
        borders.push_back(border);
        borderWidths.push_back(borderWidth);
        roundnesses.push_back(roundness);
    }

    void drawTopRoundedRect(const graphics::RectF& rect, float radius, graphics::Color color,
                            float roundness) override {
        topRoundedRects.push_back(rect);
        roundedRadii.push_back(radius);
        colors.push_back(color);
        roundnesses.push_back(roundness);
    }

    void drawText(float x, float y, const std::string& text, graphics::Color color,
                  float fontSize, graphics::FontFamily family) override {
        textPositions.push_back({x, y, 0.0f, 0.0f});
        texts.push_back(text);
        colors.push_back(color);
        fontSizes.push_back(fontSize);
        fontFamilies.push_back(family);
    }

    float measureText(const std::string& text, float fontSize, graphics::FontFamily family) override {
        if (useSharedTextMetrics) {
            return lcl::render::text_metrics::measureText(text, fontSize, family) *
                measurementScale;
        }
        return static_cast<float>(text.size()) * fontSize * 0.6f * measurementScale;
    }

    void drawBuffer(const graphics::RectF&, int, int, const uint32_t*, int stridePixels,
                    float, float, float, bool) override {
        ++bufferDrawCount;
        bufferStrides.push_back(stridePixels);
    }

    void drawImageResource(
            const graphics::RectF& destination,
            const graphics::ImageResourceView& resource,
            float opacity, float cornerRadius, float cornerRoundness,
            bool squareTopCorners) override {
        imageResourceIds.push_back(resource.id);
        imageContentRevisions.push_back(resource.contentRevision);
        imageOpaqueValues.push_back(resource.opaque);
        drawBuffer(destination, resource.width, resource.height, resource.pixels,
                   resource.stridePixels, opacity, cornerRadius,
                   cornerRoundness, squareTopCorners);
    }

    bool initialized{false};
    bool useSharedTextMetrics{false};
    float measurementScale{1.0f};
    uint32_t* pixels{nullptr};
    uint32_t pixelWidth{0};
    uint32_t pixelHeight{0};
    float deviceScale{1.0f};
    graphics::RenderTarget target{};
    int beginCount{0};
    int endCount{0};
    int bufferDrawCount{0};
    int pathDrawCount{0};
    int targetSetCount{0};
    int cachedLayerBeginCount{0};
    int cachedLayerUpdateBeginCount{0};
    int cachedLayerEndCount{0};
    int cachedLayerDrawCount{0};
    CachedLayerId activeCachedLayer{0};
    bool dmaBufAvailable{false};
    bool dmaBufEnabled{false};
    uint32_t dmaContentWidth{0};
    uint32_t dmaContentHeight{0};
    uint32_t dmaBackingWidth{0};
    uint32_t dmaBackingHeight{0};
    uint32_t dmaCapacityWidth{0};
    uint32_t dmaCapacityHeight{0};
    uint32_t nextBufferId{1};
    int dmaConfigureCount{0};
    int dmaCapacityGrowCount{0};
    std::vector<uint32_t> releasedBufferIds;
    std::vector<graphics::RectF> rects;
    std::vector<graphics::RectF> clearedRects;
    std::vector<graphics::RectF> roundedRects;
    std::vector<graphics::RectF> topRoundedRects;
    std::vector<graphics::RectF> textPositions;
    std::vector<graphics::RectF> clips;
    std::vector<graphics::RectF> cachedLayerBounds;
    std::vector<graphics::RectF> cachedLayerUpdateBounds;
    std::vector<graphics::RectF> cachedLayerDestinations;
    std::vector<graphics::Matrix3> cachedLayerTransforms;
    std::vector<float> roundedRadii;
    std::vector<float> borderWidths;
    std::vector<float> roundnesses;
    std::vector<float> fontSizes;
    std::vector<int> bufferStrides;
    std::vector<uint64_t> imageResourceIds;
    std::vector<uint64_t> imageContentRevisions;
    std::vector<bool> imageOpaqueValues;
    std::vector<graphics::FontFamily> fontFamilies;
    std::vector<graphics::Color> colors;
    std::vector<graphics::Color> clearColors;
    std::vector<graphics::Color> borders;
    std::vector<std::string> texts;
    std::unordered_set<CachedLayerId> cachedLayers;
};

class CountingLayoutContainer final : public Container {
public:
    void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f) override {
        ++syncLayoutCount;
        Container::syncLayout(parentAbsX, parentAbsY);
    }

    int syncLayoutCount{0};
};

class CountingPaintWidget final : public Widget {
public:
    void draw(graphics::Canvas& canvas, const graphics::RectF&) override {
        ++paintCount;
        canvas.drawRect(m_absoluteBounds, {20, 40, 60, 255});
    }

    int paintCount{0};
};

class PointerProbeWidget final : public Widget {
public:
    explicit PointerProbeWidget(graphics::Color color = {0, 0, 0, 0}) : m_color(color) {}

    void draw(graphics::Canvas& canvas, const graphics::RectF&) override {
        ++paintCount;
        canvas.drawRect(m_absoluteBounds, m_color);
    }

    bool onPointerDown(const PointerEvent& event) override {
        ++pointerDownCount;
        if (captureOnDown) event.capturePointer(*this);
        return true;
    }
    bool onPointerUp(const PointerEvent&) override { ++pointerUpCount; return true; }
    bool onPointerCancel(const PointerEvent&) override {
        ++pointerCancelCount;
        if (cancelCountSink) ++*cancelCountSink;
        return true;
    }

    graphics::Color m_color;
    bool captureOnDown{false};
    int* cancelCountSink{nullptr};
    int paintCount{0};
    int pointerDownCount{0};
    int pointerUpCount{0};
    int pointerCancelCount{0};
};

} // namespace

TEST(LclGraphicsTest, DisplayListKeepsLogicalGeometryAcrossDeviceScales) {
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const uint32_t pixelWidth = static_cast<uint32_t>(std::ceil(40.0f * scale));
        const uint32_t pixelHeight = static_cast<uint32_t>(std::ceil(30.0f * scale));
        std::vector<uint32_t> pixels(
            static_cast<size_t>(pixelWidth) * pixelHeight, 0u);
        lcl::render::RasterRenderer renderer;
        ASSERT_TRUE(renderer.initialize(pixelWidth, pixelHeight, nullptr, pixels.data()));
        lcl::render::RasterCanvas canvas(renderer);
        canvas.setRenderTarget({{40.0f, 30.0f}, {pixelWidth, pixelHeight}, scale});

        graphics::Path path;
        path.addRRect({{3.25f, 4.5f, 20.0f, 12.0f}, 4.0f, 4.0f, 2.0f});
        graphics::Paint paint{};
        paint.color = {255, 255, 255, 255};
        paint.style = graphics::PaintStyle::Stroke;
        paint.stroke.width = 1.0f;

        canvas.beginFrame();
        canvas.drawPath(path, paint);
        canvas.endFrame();

        ASSERT_EQ(canvas.lastDisplayList().commands().size(), 1u);
        const auto* draw = std::get_if<graphics::DrawPathCommand>(
            &canvas.lastDisplayList().commands().front());
        ASSERT_NE(draw, nullptr);
        EXPECT_FLOAT_EQ(draw->paint.stroke.width, 1.0f);
        EXPECT_EQ(draw->paint.stroke.scaling,
                  graphics::StrokeScaling::ScaleWithTransform);
        ASSERT_FALSE(draw->path.elements().empty());
        EXPECT_FLOAT_EQ(draw->path.elements().front().p0.x, 7.25f);
        EXPECT_FLOAT_EQ(draw->path.elements().front().p0.y, 4.5f);
    }
}

TEST(LclGraphicsTest, RasterCanvasDefersRasterWorkUntilDisplayListReplay) {
    std::vector<uint32_t> pixels(16 * 16, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(16, 16, nullptr, pixels.data()));
    renderer.setRetainsFrameBacking(true);
    lcl::render::RasterCanvas canvas(renderer);

    canvas.beginFrame();
    canvas.drawRect({2.0f, 3.0f, 6.0f, 5.0f}, {255, 255, 255, 255});
    EXPECT_EQ(pixels[4 + 4 * 16], 0x00000000u);
    canvas.endFrame();

    EXPECT_EQ(pixels[4 + 4 * 16], 0xFFFFFFFFu);
    ASSERT_EQ(canvas.lastDisplayList().commands().size(), 1u);
    EXPECT_NE(std::get_if<graphics::DrawPathCommand>(
                  &canvas.lastDisplayList().commands().front()), nullptr);
}

TEST(LclGraphicsTest, CompositorOwnedDisplayListLayerRasterizesThenComposes) {
    std::vector<uint32_t> scenePixels(16 * 16, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(16, 16, nullptr, scenePixels.data()));

    graphics::DisplayListBuilder builder;
    builder.clearRect({0.0f, 0.0f, 4.0f, 4.0f}, {0, 0, 0, 0});
    builder.drawPath([] {
        graphics::Path path;
        path.addRect({0.0f, 0.0f, 4.0f, 4.0f});
        return path;
    }(), graphics::Paint{{255, 0, 0, 255}});
    const graphics::RenderTarget target{{4.0f, 4.0f}, {4, 4}, 1.0f};
    ASSERT_TRUE(renderer.updateCachedDisplayLayer(
        91, {0.0f, 0.0f, 4.0f, 4.0f}, builder.build(), target));
    EXPECT_EQ(scenePixels[3 + 4 * 16], 0x00000000u);

    ASSERT_TRUE(renderer.drawCachedDisplayLayer(
        91, {2.0f, 3.0f, 4.0f, 4.0f}));
    EXPECT_EQ(scenePixels[3 + 4 * 16], 0xFFFF0000u);
    renderer.releaseCachedDisplayLayer(91);
    EXPECT_FALSE(renderer.hasCachedDisplayLayer(91));
}

TEST(LclGraphicsTest, RasterCanvasRecordsClearLayerImageAndTextCommands) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    std::array<uint32_t, 6> imagePixels{
        0xFFFFFFFFu, 0xFFFFFFFFu, 0u,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0u,
    };
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    renderer.setRetainsFrameBacking(true);
    lcl::render::RasterCanvas canvas(renderer);

    canvas.beginFrame();
    canvas.clearRect({0.0f, 0.0f, 32.0f, 32.0f}, {0, 0, 0, 0});
    canvas.beginLayer(0.5f);
    canvas.drawBuffer({10.0f, 10.0f, 2.0f, 2.0f},
                      2, 2, imagePixels.data(), 3,
                      1.0f, 0.0f, 2.0f, false);
    canvas.drawText(1.0f, 1.0f, "A", {255, 255, 255, 255},
                    10.0f, graphics::FontFamily::Interface);
    canvas.endLayer();
    canvas.endFrame();

    const auto& commands = canvas.lastDisplayList().commands();
    ASSERT_EQ(commands.size(), 5u);
    EXPECT_NE(std::get_if<graphics::ClearRectCommand>(&commands[0]), nullptr);
    EXPECT_NE(std::get_if<graphics::BeginLayerCommand>(&commands[1]), nullptr);
    const auto* image = std::get_if<graphics::DrawImageCommand>(&commands[2]);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->stridePixels, 3);
    const auto* text = std::get_if<graphics::DrawTextCommand>(&commands[3]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "A");
    EXPECT_NE(std::get_if<graphics::EndLayerCommand>(&commands[4]), nullptr);
}

TEST(LclGraphicsTest, RasterCanvasRecordsStableImageResourceMetadata) {
    std::vector<uint32_t> targetPixels(8 * 8, 0x00000000u);
    const std::array<uint32_t, 4> imagePixels{
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(8, 8, nullptr, targetPixels.data()));
    renderer.setRetainsFrameBacking(true);
    lcl::render::RasterCanvas canvas(renderer);

    canvas.beginFrame();
    canvas.drawImageResource(
        {1.0f, 1.0f, 2.0f, 2.0f},
        {73, 4, 2, 2, imagePixels.data(), 2, true},
        1.0f, 0.0f, 2.0f, false);
    canvas.endFrame();

    const auto& commands = canvas.lastDisplayList().commands();
    ASSERT_EQ(commands.size(), 1u);
    const auto* image = std::get_if<graphics::DrawImageCommand>(&commands.front());
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->resourceId, 73u);
    EXPECT_EQ(image->contentRevision, 4u);
    EXPECT_TRUE(image->opaque);
}

TEST(LclGraphicsTest, CanvasHelperPathsRetainGpuPrimitiveMetadata) {
    graphics::Path rect;
    rect.addRect({2.0f, 3.0f, 80.0f, 40.0f});
    ASSERT_NE(rect.primitive(), nullptr);
    EXPECT_EQ(rect.primitive()->kind, graphics::PathPrimitiveKind::Rect);

    graphics::Path rounded;
    rounded.addRRect({{4.0f, 5.0f, 120.0f, 60.0f}, 12.0f, 12.0f, 2.0f});
    ASSERT_NE(rounded.primitive(), nullptr);
    EXPECT_EQ(rounded.primitive()->kind, graphics::PathPrimitiveKind::RRect);

    graphics::Path topRounded;
    topRounded.addTopRRect({6.0f, 7.0f, 160.0f, 32.0f}, 10.0f, 2.0f);
    ASSERT_NE(topRounded.primitive(), nullptr);
    EXPECT_EQ(topRounded.primitive()->kind,
              graphics::PathPrimitiveKind::TopRRect);

    graphics::Path generic;
    generic.moveTo(0.0f, 0.0f).lineTo(20.0f, 0.0f)
        .lineTo(10.0f, 20.0f).close();
    EXPECT_EQ(generic.primitive(), nullptr);

    rect.lineTo(100.0f, 100.0f);
    EXPECT_EQ(rect.primitive(), nullptr);
}

TEST(LclGraphicsTest, CanvasDisplayListReplayPreservesImageStride) {
    RecordingCanvas canvas;
    const uint32_t pixels[8]{};
    graphics::DisplayListBuilder builder;
    builder.drawImage({1.0f, 2.0f, 2.0f, 2.0f},
                      reinterpret_cast<uintptr_t>(pixels),
                      2, 2, 4, 1.0f, 0.0f, 2.0f, false);

    canvas.drawDisplayList(builder.build());

    EXPECT_EQ(canvas.bufferDrawCount, 1);
    ASSERT_EQ(canvas.bufferStrides.size(), 1u);
    EXPECT_EQ(canvas.bufferStrides.front(), 4);
}

TEST(LclGraphicsTest, CanvasDisplayListReplayPreservesImageResourceIdentity) {
    RecordingCanvas canvas;
    const uint32_t pixels[4]{0xFFFFFFFFu, 0xFFFFFFFFu,
                             0xFFFFFFFFu, 0xFFFFFFFFu};
    graphics::DisplayListBuilder builder;
    builder.drawImage({1.0f, 2.0f, 2.0f, 2.0f},
                      reinterpret_cast<uintptr_t>(pixels),
                      2, 2, 2, 1.0f, 0.0f, 2.0f, false,
                      42, 7, true);

    canvas.drawDisplayList(builder.build());

    ASSERT_EQ(canvas.imageResourceIds.size(), 1u);
    EXPECT_EQ(canvas.imageResourceIds.front(), 42u);
    EXPECT_EQ(canvas.imageContentRevisions.front(), 7u);
    EXPECT_TRUE(canvas.imageOpaqueValues.front());
}

TEST(LclGraphicsTest, NormalStrokeScalesButHairlineRemainsOneDevicePixel) {
    graphics::Path path;
    path.addRect({4.0f, 4.0f, 8.0f, 8.0f});
    graphics::Paint normal{};
    normal.color = {255, 255, 255, 255};
    normal.style = graphics::PaintStyle::Stroke;
    normal.stroke.width = 2.0f;
    graphics::Paint hairline = normal;
    hairline.stroke.scaling = graphics::StrokeScaling::Hairline;

    const auto transform = graphics::Matrix3::scale(2.0f, 2.0f);
    const auto normalPixels = lcl::render::rasterizePath(path, normal, transform);
    const auto hairlinePixels = lcl::render::rasterizePath(path, hairline, transform);
    ASSERT_FALSE(normalPixels.empty());
    ASSERT_FALSE(hairlinePixels.empty());
    EXPECT_GT(normalPixels.width, hairlinePixels.width);
    EXPECT_GT(normalPixels.height, hairlinePixels.height);
}

TEST(LclGraphicsTest, DamageConversionUsesFloorMinAndCeilMax) {
    const auto device = graphics::enclosingDeviceRect(
        {1.1f, 2.2f, 3.3f, 4.4f}, graphics::Matrix3::scale(1.25f, 1.25f));
    EXPECT_FLOAT_EQ(device.x, 1.0f);
    EXPECT_FLOAT_EQ(device.y, 2.0f);
    EXPECT_FLOAT_EQ(device.width, 5.0f);
    EXPECT_FLOAT_EQ(device.height, 7.0f);
}

TEST(LclGraphicsTest, SvgStyleArcFlattensToCurvedEndpointGeometry) {
    graphics::Path path;
    path.moveTo(0.0f, 0.0f)
        .arcTo(10.0f, 10.0f, 0.0f, false, true, 20.0f, 0.0f);
    const auto contours = graphics::flattenPath(path, graphics::Matrix3::identity());
    ASSERT_EQ(contours.size(), 1u);
    ASSERT_GT(contours.front().points.size(), 2u);
    EXPECT_NEAR(contours.front().points.front().x, 0.0f, 0.001f);
    EXPECT_NEAR(contours.front().points.front().y, 0.0f, 0.001f);
    EXPECT_NEAR(contours.front().points.back().x, 20.0f, 0.001f);
    EXPECT_NEAR(contours.front().points.back().y, 0.0f, 0.001f);
}

TEST(LclUiTest, RectMath) {
    graphics::RectF r1{10.0f, 10.0f, 50.0f, 50.0f};
    graphics::RectF r2{30.0f, 30.0f, 50.0f, 50.0f};

    EXPECT_TRUE(r1.intersects(r2));
    EXPECT_TRUE(r1.containsPoint(20.0f, 20.0f));
    EXPECT_FALSE(r1.containsPoint(70.0f, 70.0f));

    graphics::RectF intersection = r1.intersection(r2);
    EXPECT_EQ(intersection.x, 30.0f);
    EXPECT_EQ(intersection.y, 30.0f);
    EXPECT_EQ(intersection.width, 30.0f);
    EXPECT_EQ(intersection.height, 30.0f);

    graphics::RectF unionRect = r1.unionWith(r2);
    EXPECT_EQ(unionRect.x, 10.0f);
    EXPECT_EQ(unionRect.y, 10.0f);
    EXPECT_EQ(unionRect.width, 70.0f);
    EXPECT_EQ(unionRect.height, 70.0f);
}
TEST(LclUiTest, WidgetsUseBackendNeutralCanvas) {
    RecordingCanvas canvas;
    auto root = std::make_unique<Container>();
    root->setBackgroundColor({10, 20, 30, 255});
    root->setWidth(120.0f);
    root->setHeight(80.0f);

    auto label = std::make_unique<Text>("graphics::Canvas");
    label->setTextColor({230, 231, 232, 255});
    root->addChild(std::move(label));

    root->calculateLayout(120.0f, 80.0f);
    root->syncLayout();
    root->draw(canvas, {0.0f, 0.0f, 120.0f, 80.0f});

    ASSERT_EQ(canvas.rects.size(), 1u);
    EXPECT_EQ(canvas.colors.front().r, 10);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.front(), "graphics::Canvas");
}

TEST(LclUiTest, TextIntrinsicMeasurementMatchesRendererGlyphAdvances) {
    std::vector<uint32_t> pixels(512 * 96, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(512, 96, nullptr, pixels.data()));

    for (const std::string& value : {"iiiiiiii", "WWWWWWWW", "ScrollView Test Paneli"}) {
        Text text(value);
        text.setFontSize(18.0f);
        text.calculateLayout();
        text.syncLayout();

        // The layout engine rounds edges to physical pixels while glyph advances
        // remain fractional inside that one-pixel envelope.
        EXPECT_NEAR(text.getBounds().width, renderer.measureString(value, 18.0f), 1.0f)
            << value;
    }
}

TEST(LclUiTest, FlexCenteredTextUsesItsMeasuredGlyphWidthForOrigin) {
    std::vector<uint32_t> pixels(400 * 96, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(400, 96, nullptr, pixels.data()));

    for (const std::string& value : {"iiiiiiii", "WWWWWWWW", "ScrollView Test Paneli"}) {
        auto root = std::make_unique<Container>();
        root->setWidth(400.0f);
        root->setHeight(96.0f);
        root->setAlignItems(layout::Align::Center);

        auto label = std::make_unique<Text>(value);
        label->setFontSize(18.0f);
        Text* labelPtr = label.get();
        root->addChild(std::move(label));

        root->calculateLayout(400.0f, 96.0f);
        root->syncLayout();

        const float renderedWidth = renderer.measureString(value, 18.0f);
        EXPECT_NEAR(labelPtr->getAbsoluteBounds().x, (400.0f - renderedWidth) * 0.5f, 1.0f)
            << value;

        RecordingCanvas canvas;
        root->draw(canvas, graphics::RectF{0.0f, 0.0f, 400.0f, 96.0f});
        ASSERT_EQ(canvas.textPositions.size(), 1u);
        EXPECT_NEAR(canvas.textPositions.front().x, labelPtr->getAbsoluteBounds().x, 0.01f)
            << value;
    }
}

TEST(LclUiTest, TextMeasurementUpdatesAfterContentAndFamilyChanges) {
    std::vector<uint32_t> pixels(512 * 96, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(512, 96, nullptr, pixels.data()));

    Text text("iiiiiiii");
    text.setFontSize(18.0f);
    text.calculateLayout();
    text.syncLayout();
    const float narrowWidth = text.getBounds().width;

    text.setText("WWWWWWWW");
    text.calculateLayout();
    text.syncLayout();
    const float wideWidth = text.getBounds().width;
    EXPECT_NE(wideWidth, narrowWidth);
    EXPECT_NEAR(wideWidth, renderer.measureString("WWWWWWWW", 18.0f), 1.0f);

    text.setFontSize(24.0f);
    text.calculateLayout();
    text.syncLayout();
    EXPECT_NEAR(text.getBounds().width, renderer.measureString("WWWWWWWW", 24.0f), 1.0f);

    text.setFontFamily(graphics::FontFamily::Monospace);
    text.calculateLayout();
    text.syncLayout();
    EXPECT_NEAR(text.getBounds().width,
                renderer.measureMonospaceString("WWWWWWWW", 24.0f), 1.0f);
}

TEST(LclUiTest, TextFieldPlaceholderAndCaretFollowFocusAndValueState) {
    TextField field;
    field.setPlaceholder("Search");
    field.setWidth(180.0f);
    field.calculateLayout(180.0f, 36.0f);
    field.syncLayout();
    const graphics::RectF damage{-10.0f, -10.0f, 220.0f, 80.0f};

    RecordingCanvas canvas;
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.back(), "Search");
    EXPECT_TRUE(canvas.rects.empty());

    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    canvas.texts.clear();
    canvas.rects.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.back(), "Search");
    ASSERT_EQ(canvas.rects.size(), 1u);
    EXPECT_FLOAT_EQ(canvas.rects.back().width, 1.0f);

    field.setText("Value");
    canvas.texts.clear();
    canvas.rects.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts.back(), "Value");

    field.onFocusLost(FocusEvent{FocusEventType::Lost});
    canvas.rects.clear();
    field.draw(canvas, damage);
    EXPECT_TRUE(canvas.rects.empty());
}

TEST(LclUiTest, CaretPresentationControllerUsesDeterministicHardBlinkPhases) {
    CaretPresentationController caret;
    EXPECT_FALSE(caret.isActive());
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);

    EXPECT_TRUE(caret.setActive(true));
    EXPECT_TRUE(caret.isActive());
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_FALSE(caret.update(0.49f));
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_TRUE(caret.update(0.02f));
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);
    EXPECT_TRUE(caret.update(CaretPresentationController::kHiddenPhaseDurationSec));
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_TRUE(caret.update(CaretPresentationController::kVisiblePhaseDurationSec));
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);

    EXPECT_TRUE(caret.resetActivity());
    EXPECT_FLOAT_EQ(caret.opacity(), 1.0f);
    EXPECT_FALSE(caret.update(0.49f));
    EXPECT_TRUE(caret.update(0.02f));
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);

    EXPECT_TRUE(caret.setActive(false));
    EXPECT_FALSE(caret.isActive());
    EXPECT_FLOAT_EQ(caret.opacity(), 0.0f);
    EXPECT_FALSE(caret.update(1.0f));
}

TEST(LclUiTest, MotionCoordinatorPresentationRegistryTicksOnlyLiveEntries) {
    MotionCoordinator coordinator;
    int firstUpdates = 0;
    int secondUpdates = 0;

    auto first = std::make_unique<Widget>();
    auto second = std::make_unique<Widget>();
    first->setMotionCoordinator(&coordinator);
    second->setMotionCoordinator(&coordinator);
    coordinator.registerPresentation(*first, [&](float) { ++firstUpdates; });
    coordinator.registerPresentation(*second, [&](float) { ++secondUpdates; });

    EXPECT_TRUE(coordinator.hasActiveAnimations());
    EXPECT_TRUE(coordinator.tick(0.1f));
    EXPECT_EQ(firstUpdates, 1);
    EXPECT_EQ(secondUpdates, 1);

    coordinator.unregisterPresentation(second->getObjectId());
    EXPECT_TRUE(coordinator.tick(0.1f));
    EXPECT_EQ(firstUpdates, 2);
    EXPECT_EQ(secondUpdates, 1);

    first.reset();
    EXPECT_FALSE(coordinator.hasActiveAnimations());
    EXPECT_FALSE(coordinator.tick(0.1f));
}

TEST(LclUiTest, MotionCoordinatorReportsPresentationOwnerAsAnimating) {
    MotionCoordinator coordinator;
    auto widget = std::make_unique<Widget>();
    widget->setMotionCoordinator(&coordinator);

    coordinator.registerPresentation(*widget, [](float) {});
    EXPECT_TRUE(coordinator.isObjectAnimating(widget->getObjectId()));

    coordinator.unregisterPresentation(widget->getObjectId());
    EXPECT_FALSE(coordinator.isObjectAnimating(widget->getObjectId()));
}

TEST(LclUiTest, MotionCoordinatorForwardsInteractiveRetargetWithoutLocalTick) {
    MotionCoordinator coordinator;
    Widget widget;
    widget.setMotionCoordinator(&coordinator);
    bool submitted = false;
    coordinator.setCompositorAnimationDelegate(
        [&](Widget& target, AnimatableProperty property, float start,
            float destination, float velocity, const lcl::motion::Motion& motion) {
            submitted = &target == &widget &&
                property == AnimatableProperty::TranslationX &&
                start == 0.0f && destination == 48.0f && velocity == 0.0f &&
                motion.mode == lcl::motion::MotionMode::Tween;
            return true;
        });

    EXPECT_TRUE(coordinator.updateCompositorFloat(
        widget, AnimatableProperty::TranslationX, 0.0f, 48.0f,
        [&widget](float value) {
            widget.applyPresentationValue(
                AnimatableProperty::TranslationX, value);
        }));
    EXPECT_TRUE(submitted);
    EXPECT_FLOAT_EQ(widget.getPresentationState().translationX, 48.0f);
    EXPECT_FALSE(coordinator.hasActiveAnimations());
}

TEST(LclUiTest, IndeterminateProgressUsesPresentationInvalidation) {
    RecordingCanvas canvas;
    RenderPass pass;
    MotionCoordinator coordinator;
    ProgressView progress;
    progress.setRenderPass(&pass);
    progress.setMotionCoordinator(&coordinator);
    progress.calculateLayout(22.0f, 22.0f);
    progress.syncLayout();
    progress.draw(canvas, {-10.0f, -10.0f, 42.0f, 42.0f});
    pass.clear();

    const uint64_t paintRevision = progress.getPaintRevision();
    const uint64_t presentationRevision = progress.getPresentationRevision();
    ASSERT_TRUE(coordinator.isObjectAnimating(progress.getObjectId()));

    coordinator.tick(1.0f / 60.0f);

    EXPECT_EQ(progress.getPaintRevision(), paintRevision);
    EXPECT_GT(progress.getPresentationRevision(), presentationRevision);
    EXPECT_TRUE(pass.hasDamage());
}

TEST(LclUiTest, TextFieldCaretPresentationResetsForEditingAndCaretActivity) {
    TextField field("abc");
    MotionCoordinator coordinator;
    field.setMotionCoordinator(&coordinator);
    field.setWidth(180.0f);
    field.calculateLayout(180.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    const graphics::RectF damage{-10.0f, -10.0f, 220.0f, 80.0f};
    RecordingCanvas canvas;

    auto expectVisibleCaret = [&] {
        canvas.rects.clear();
        canvas.colors.clear();
        field.draw(canvas, damage);
        ASSERT_EQ(canvas.rects.size(), 1u);
        ASSERT_FALSE(canvas.colors.empty());
        EXPECT_EQ(canvas.colors.back().a, 255);
    };
    auto expectHiddenCaret = [&] {
        canvas.rects.clear();
        field.draw(canvas, damage);
        EXPECT_TRUE(canvas.rects.empty());
    };

    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onTextInput(TextInputEvent{"ç"}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::ArrowLeft,
                                         static_cast<int>(lcl::platform::PhysicalKey::ArrowLeft)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::ArrowRight,
                                         static_cast<int>(lcl::platform::PhysicalKey::ArrowRight)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                         static_cast<int>(lcl::platform::PhysicalKey::Home)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::End,
                                         static_cast<int>(lcl::platform::PhysicalKey::End)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Backspace,
                                         static_cast<int>(lcl::platform::PhysicalKey::Backspace)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                         static_cast<int>(lcl::platform::PhysicalKey::Home)}));
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Delete,
                                         static_cast<int>(lcl::platform::PhysicalKey::Delete)}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    ASSERT_TRUE(field.onPointerDown(PointerEvent{
        18.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
        PointerSource::Mouse, 0}));
    expectVisibleCaret();
    EXPECT_TRUE(coordinator.tick(0.5f));
    expectHiddenCaret();

    field.setText("reset");
    expectVisibleCaret();
    field.onFocusLost(FocusEvent{FocusEventType::Lost});
    expectHiddenCaret();
    EXPECT_FALSE(coordinator.hasActiveAnimations());
}

TEST(LclUiTest, WindowAppSchedulesCaretPresentationOnlyWhileFieldIsFocused) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 180, 36, "Caret Presentation Test");

    auto field = std::make_unique<TextField>();
    field->setWidth(180.0f);
    TextField* fieldPtr = field.get();
    app.setRootWidget(std::move(field));
    app.updateLayout();

    EXPECT_FALSE(app.hasActiveAnimations());
    ASSERT_TRUE(app.sendPointerDown(10.0f, 10.0f));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), fieldPtr);
    EXPECT_TRUE(app.hasActiveAnimations());
    EXPECT_TRUE(app.advanceAnimations(0.5f));

    recorded->rects.clear();
    fieldPtr->draw(*recorded, {-10.0f, -10.0f, 220.0f, 80.0f});
    EXPECT_TRUE(recorded->rects.empty());

    ASSERT_TRUE(app.sendPointerDown(10.0f, 10.0f, 0, PointerSource::Touch, 7));
    ASSERT_TRUE(app.sendPointerUp(10.0f, 10.0f, 0, PointerSource::Touch, 7));
    recorded->rects.clear();
    recorded->colors.clear();
    fieldPtr->draw(*recorded, {-10.0f, -10.0f, 220.0f, 80.0f});
    ASSERT_EQ(recorded->rects.size(), 1u);
    EXPECT_EQ(recorded->colors.back().a, 255);

    app.getDispatcher().setFocus(nullptr);
    EXPECT_FALSE(app.hasActiveAnimations());
}

TEST(LclUiTest, TextFieldFocusTransferMovesTheActiveCaretPresentation) {
    MotionCoordinator coordinator;
    TextField first("first");
    TextField second("second");
    first.setMotionCoordinator(&coordinator);
    second.setMotionCoordinator(&coordinator);
    for (TextField* field : {&first, &second}) {
        field->setWidth(180.0f);
        field->calculateLayout(180.0f, 36.0f);
        field->syncLayout();
    }

    EventDispatcher dispatcher;
    const graphics::RectF damage{-10.0f, -10.0f, 220.0f, 80.0f};
    RecordingCanvas canvas;
    dispatcher.setFocus(&first);
    EXPECT_TRUE(coordinator.hasActiveAnimations());
    EXPECT_TRUE(coordinator.tick(0.5f));
    first.draw(canvas, damage);
    EXPECT_TRUE(canvas.rects.empty());

    dispatcher.setFocus(&second);
    canvas.rects.clear();
    second.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    EXPECT_TRUE(coordinator.tick(0.5f));
    canvas.rects.clear();
    second.draw(canvas, damage);
    EXPECT_TRUE(canvas.rects.empty());

    dispatcher.setFocus(nullptr);
    EXPECT_FALSE(coordinator.hasActiveAnimations());
}

TEST(LclUiTest, CaretPhaseChangesProduceDamageButIntermediateTicksDoNot) {
    MotionCoordinator coordinator;
    RenderPass pass;
    TextField field;
    field.setMotionCoordinator(&coordinator);
    field.setRenderPass(&pass);
    field.setWidth(180.0f);
    field.calculateLayout(180.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    pass.clear();

    EXPECT_TRUE(coordinator.tick(0.49f));
    EXPECT_FALSE(pass.hasDamage());
    EXPECT_TRUE(coordinator.tick(0.02f));
    EXPECT_TRUE(pass.hasDamage());
    pass.clear();

    EXPECT_TRUE(coordinator.tick(0.1f));
    EXPECT_FALSE(pass.hasDamage());
    EXPECT_TRUE(coordinator.tick(0.4f));
    EXPECT_TRUE(pass.hasDamage());
}

TEST(LclUiTest, TextFieldCaretUsesActiveCanvasProportionalMetrics) {
    TextField field;
    field.setWidth(240.0f);
    field.calculateLayout(240.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    const graphics::RectF damage{-10.0f, -10.0f, 280.0f, 80.0f};

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    canvas.measurementScale = 0.5f;
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    const float emptyCaretX = canvas.rects.back().x;

    field.setText("Wi");
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::End,
                                         static_cast<int>(lcl::platform::PhysicalKey::End)}));
    canvas.rects.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    const float expectedAdvance = lcl::render::text_metrics::measureText(
        "Wi", 14.0f, graphics::FontFamily::Interface) * canvas.measurementScale;
    EXPECT_NEAR(canvas.rects.back().x - emptyCaretX, expectedAdvance, 0.01f);
}

TEST(LclUiTest, TextFieldHorizontallyScrollsLongTextAndClipsCaret) {
    const std::string value = "abcçdefşğıİöüǩžʒ";
    TextField field(value);
    field.setWidth(56.0f);
    field.calculateLayout(56.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    field.draw(canvas, {-10.0f, -10.0f, 100.0f, 80.0f});

    ASSERT_FALSE(canvas.clips.empty());
    ASSERT_FALSE(canvas.textPositions.empty());
    ASSERT_EQ(canvas.rects.size(), 1u);
    const graphics::RectF viewport = canvas.clips.back();
    const graphics::RectF caret = canvas.rects.back();
    EXPECT_LT(canvas.textPositions.back().x, viewport.x);
    EXPECT_GE(caret.x, viewport.x);
    EXPECT_LE(caret.x + caret.width, viewport.x + viewport.width + 0.01f);
    EXPECT_NEAR(caret.x - canvas.textPositions.back().x,
                lcl::render::text_metrics::measureText(
                    value, 14.0f, graphics::FontFamily::Interface),
                0.01f);
}

TEST(LclUiTest, TextFieldCaretWalkUsesCodepointPrefixesForAsciiAndUtf8) {
    const std::vector<std::vector<std::string>> prefixSets{
        {"", "a", "ab", "abc", "abcd", "abcde", "abcdef"},
        {"", "a", "ab", "abc", "abcç", "abcçd", "abcçde", "abcçdef"},
        {"", "ş", "şg", "şğı", "şğıİ", "şğıİö", "şğıİöü"},
        {"", "ǩ", "ǩž", "ǩžʒ"},
    };
    const graphics::RectF damage{-10.0f, -10.0f, 700.0f, 80.0f};

    for (const auto& prefixes : prefixSets) {
        const std::string& value = prefixes.back();
        SCOPED_TRACE(value);
        TextField field(value);
        field.setWidth(640.0f);
        field.calculateLayout(640.0f, 36.0f);
        field.syncLayout();
        field.onFocusGained(FocusEvent{FocusEventType::Gained});
        ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                             static_cast<int>(lcl::platform::PhysicalKey::Home)}));

        RecordingCanvas canvas;
        canvas.useSharedTextMetrics = true;
        for (size_t index = 0; index < prefixes.size(); ++index) {
            SCOPED_TRACE(index);
            canvas.rects.clear();
            canvas.textPositions.clear();
            field.draw(canvas, damage);
            ASSERT_EQ(canvas.rects.size(), 1u);
            ASSERT_EQ(canvas.textPositions.size(), 1u);
            EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                        lcl::render::text_metrics::measureText(
                            prefixes[index], 14.0f, graphics::FontFamily::Interface),
                        0.01f);
            if (index + 1 < prefixes.size()) {
                ASSERT_TRUE(field.onKeyDown(KeyEvent{
                    lcl::platform::PhysicalKey::ArrowRight,
                    static_cast<int>(lcl::platform::PhysicalKey::ArrowRight)}));
            }
        }

        for (size_t index = prefixes.size(); index-- > 1;) {
            ASSERT_TRUE(field.onKeyDown(KeyEvent{
                lcl::platform::PhysicalKey::ArrowLeft,
                static_cast<int>(lcl::platform::PhysicalKey::ArrowLeft)}));
            canvas.rects.clear();
            canvas.textPositions.clear();
            field.draw(canvas, damage);
            ASSERT_EQ(canvas.rects.size(), 1u);
            ASSERT_EQ(canvas.textPositions.size(), 1u);
            EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                        lcl::render::text_metrics::measureText(
                            prefixes[index - 1], 14.0f, graphics::FontFamily::Interface),
                        0.01f);
        }
    }
}

TEST(LclUiTest, TextFieldPointerCaretPositionUsesUtf8CodepointBoundaries) {
    TextField field("abcçdef");
    field.setWidth(300.0f);
    field.calculateLayout(300.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    const graphics::RectF damage{-10.0f, -10.0f, 340.0f, 80.0f};

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.textPositions.size(), 1u);
    const float prefixWidth = lcl::render::text_metrics::measureText(
        "abcç", 14.0f, graphics::FontFamily::Interface);
    const float pointerX = canvas.textPositions.back().x + prefixWidth;

    ASSERT_TRUE(field.onPointerDown(PointerEvent{
        pointerX, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down,
        PointerSource::Mouse, 0}));
    canvas.rects.clear();
    canvas.textPositions.clear();
    field.draw(canvas, damage);
    ASSERT_EQ(canvas.rects.size(), 1u);
    ASSERT_EQ(canvas.textPositions.size(), 1u);
    EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                prefixWidth, 0.01f);
}

TEST(LclUiTest, TextFieldUtf8InsertionLeavesCaretAfterInsertedCodepoints) {
    TextField field("aç");
    field.setWidth(300.0f);
    field.calculateLayout(300.0f, 36.0f);
    field.syncLayout();
    field.onFocusGained(FocusEvent{FocusEventType::Gained});
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::Home,
                                         static_cast<int>(lcl::platform::PhysicalKey::Home)}));
    ASSERT_TRUE(field.onKeyDown(KeyEvent{lcl::platform::PhysicalKey::ArrowRight,
                                         static_cast<int>(lcl::platform::PhysicalKey::ArrowRight)}));
    ASSERT_TRUE(field.onTextInput(TextInputEvent{"ş"}));
    ASSERT_TRUE(field.onTextInput(TextInputEvent{"ğ"}));

    RecordingCanvas canvas;
    canvas.useSharedTextMetrics = true;
    field.draw(canvas, {-10.0f, -10.0f, 340.0f, 80.0f});
    ASSERT_EQ(canvas.rects.size(), 1u);
    ASSERT_EQ(canvas.textPositions.size(), 1u);
    EXPECT_NEAR(canvas.rects.back().x - canvas.textPositions.back().x,
                lcl::render::text_metrics::measureText(
                    "aşğ", 14.0f, graphics::FontFamily::Interface),
                0.01f);
}

TEST(LclUiTest, WindowAppAcceptsInjectedCanvas) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 64, 48, "Injected graphics::Canvas Test");
    ASSERT_TRUE(recorded->initialized);

    auto root = std::make_unique<Container>();
    root->setBackgroundColor({1, 2, 3, 255});
    root->setWidth(64.0f);
    root->setHeight(48.0f);
    app.setRootWidget(std::move(root));

    EXPECT_TRUE(app.renderFrame());
    EXPECT_EQ(recorded->beginCount, 1);
    EXPECT_EQ(recorded->endCount, 1);
    ASSERT_EQ(recorded->rects.size(), 1u);
    EXPECT_EQ(recorded->rects.front().width, 64.0f);
    EXPECT_EQ(recorded->rects.front().height, 48.0f);
}

TEST(LclUiTest, WindowAppCachesEligiblePresentationTransforms) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(
        std::move(canvas), 120, 80,
        "Retained presentation boundary");

    auto root = std::make_unique<Container>();
    Container* rootPointer = root.get();
    root->setBackgroundColor({30, 60, 90, 255});
    root->setScale(0.9f);
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(recorded->cachedLayerBeginCount, 1);
    EXPECT_EQ(recorded->cachedLayerEndCount, 1);
    EXPECT_EQ(recorded->cachedLayerDrawCount, 1);

    rootPointer->setTranslationX(12.0f);
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(recorded->cachedLayerBeginCount, 1);
    EXPECT_EQ(recorded->cachedLayerUpdateBeginCount, 1);
    EXPECT_EQ(recorded->cachedLayerEndCount, 2);
    EXPECT_EQ(recorded->cachedLayerDrawCount, 2);
}

TEST(LclUiTest, LocalTransientRendersAboveContentInSingleWindowRootLayout) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 64, 48, "Overlay render order");

    auto root = std::make_unique<Container>();
    root->setWidth(64.0f);
    root->setHeight(48.0f);
    root->setBackgroundColor({10, 20, 30, 255});
    root->setAlignItems(layout::Align::Center);
    root->setJustifyContent(layout::Justify::Center);
    auto content = std::make_unique<Widget>();
    content->setWidth(10.0f);
    content->setHeight(10.0f);
    Widget* contentPtr = content.get();
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    auto overlay = std::make_unique<PointerProbeWidget>(graphics::Color{40, 50, 60, 255});
    overlay->setWidth(24.0f);
    overlay->setHeight(18.0f);
    overlay->setPosition(layout::Edge::Left, 8.0f);
    overlay->setPosition(layout::Edge::Top, 6.0f);
    PointerProbeWidget* overlayPtr = overlay.get();
    const TransientHandle handle = app.registerLocalTransient(std::move(overlay));
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(app.renderFrame());
    ASSERT_GE(recorded->colors.size(), 2u);
    EXPECT_EQ(recorded->colors[recorded->colors.size() - 2].r, 10);
    EXPECT_EQ(recorded->colors.back().r, 40);
    EXPECT_FLOAT_EQ(overlayPtr->getAbsoluteBounds().x, 8.0f);
    EXPECT_FLOAT_EQ(overlayPtr->getAbsoluteBounds().y, 6.0f);
    EXPECT_FLOAT_EQ(contentPtr->getAbsoluteBounds().x, 27.0f);
    EXPECT_FLOAT_EQ(contentPtr->getAbsoluteBounds().y, 19.0f);
    EXPECT_FLOAT_EQ(app.getRootWidget()->getAbsoluteBounds().width, 64.0f);
}

TEST(LclUiTest, LocalTransientPrioritizesTopmostHitWithoutBlockingEmptySpace) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay hit testing");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto content = std::make_unique<PointerProbeWidget>();
    content->setWidth(100.0f);
    content->setHeight(80.0f);
    PointerProbeWidget* contentPtr = content.get();
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    auto first = std::make_unique<PointerProbeWidget>();
    first->setWidth(40.0f);
    first->setHeight(30.0f);
    first->setPosition(layout::Edge::Left, 10.0f);
    first->setPosition(layout::Edge::Top, 10.0f);
    PointerProbeWidget* firstPtr = first.get();
    const TransientHandle firstHandle = app.registerLocalTransient(std::move(first));

    auto second = std::make_unique<PointerProbeWidget>();
    second->setWidth(40.0f);
    second->setHeight(30.0f);
    second->setPosition(layout::Edge::Left, 10.0f);
    second->setPosition(layout::Edge::Top, 10.0f);
    PointerProbeWidget* secondPtr = second.get();
    const TransientHandle secondHandle = app.registerLocalTransient(std::move(second));
    ASSERT_NE(firstHandle, 0u);
    ASSERT_NE(secondHandle, 0u);
    app.updateLayout();

    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(secondPtr->pointerDownCount, 1);
    EXPECT_EQ(firstPtr->pointerDownCount, 0);

    ASSERT_TRUE(app.removeTransient(secondHandle));
    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(firstPtr->pointerDownCount, 1);
    ASSERT_TRUE(app.removeTransient(firstHandle));
    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(contentPtr->pointerDownCount, 1);
}

TEST(LclUiTest, TransientControllerMouseDismissAndRemovalAreLifecycleSafe) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay dismissal");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto content = std::make_unique<PointerProbeWidget>();
    content->setWidth(100.0f);
    content->setHeight(80.0f);
    PointerProbeWidget* contentPtr = content.get();
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    int overlayCancelCount = 0;
    auto insideOverlay = std::make_unique<PointerProbeWidget>();
    insideOverlay->setFocusable(true);
    insideOverlay->captureOnDown = true;
    insideOverlay->cancelCountSink = &overlayCancelCount;
    insideOverlay->setWidth(40.0f);
    insideOverlay->setHeight(30.0f);
    insideOverlay->setPosition(layout::Edge::Left, 10.0f);
    insideOverlay->setPosition(layout::Edge::Top, 10.0f);
    PointerProbeWidget* insidePtr = insideOverlay.get();
    const TransientHandle insideHandle = app.registerLocalTransient(
        std::move(insideOverlay), TransientOptions{.dismissOnOutsidePointer = true});
    app.updateLayout();

    ASSERT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(app.getTransientController().size(), 1u);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), insidePtr);
    EXPECT_TRUE(app.getDispatcher().hasPointerCapture(0, insidePtr));
    ASSERT_TRUE(app.removeTransient(insideHandle));
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_FALSE(app.getDispatcher().hasPointerCapture(0));
    EXPECT_EQ(overlayCancelCount, 1);

    int dismissCount = 0;
    auto outsideOverlay = std::make_unique<PointerProbeWidget>();
    outsideOverlay->setWidth(40.0f);
    outsideOverlay->setHeight(30.0f);
    outsideOverlay->setPosition(layout::Edge::Left, 10.0f);
    outsideOverlay->setPosition(layout::Edge::Top, 10.0f);
    app.registerLocalTransient(std::move(outsideOverlay), TransientOptions{
        .dismissOnOutsidePointer = true,
        .onDismiss = [&] { ++dismissCount; },
    });
    app.updateLayout();

    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(dismissCount, 1);
    EXPECT_EQ(contentPtr->pointerDownCount, 1);

    auto nonDismissable = std::make_unique<PointerProbeWidget>();
    nonDismissable->setWidth(20.0f);
    nonDismissable->setHeight(20.0f);
    app.registerLocalTransient(std::move(nonDismissable));
    app.updateLayout();
    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 1u);

    // A non-dismissible topmost overlay blocks dismissal of older entries.
    app.clearTransients();
    app.registerLocalTransient(std::make_unique<PointerProbeWidget>(),
                    TransientOptions{.dismissOnOutsidePointer = true});
    app.registerLocalTransient(std::make_unique<PointerProbeWidget>());
    app.updateLayout();
    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 2u);

    app.clearTransients();
    const TransientHandle lowerHandle = app.registerLocalTransient(std::make_unique<PointerProbeWidget>());
    const TransientHandle topHandle = app.registerLocalTransient(
        std::make_unique<PointerProbeWidget>(), TransientOptions{
            .dismissOnOutsidePointer = true,
            .onDismiss = [&] { app.removeTransient(lowerHandle); },
        });
    ASSERT_NE(lowerHandle, 0u);
    ASSERT_NE(topHandle, 0u);
    app.updateLayout();
    ASSERT_TRUE(app.sendPointerDown(90.0f, 70.0f));
    EXPECT_EQ(app.getTransientController().size(), 0u);
}

TEST(LclUiTest, TransientControllerTouchDismissRequiresValidatedTap) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay touch dismissal");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto content = std::make_unique<PointerProbeWidget>();
    content->setWidth(100.0f);
    content->setHeight(80.0f);
    root->addChild(std::move(content));
    app.setRootWidget(std::move(root));

    auto addDismissable = [&] {
        auto overlay = std::make_unique<PointerProbeWidget>();
        overlay->setWidth(20.0f);
        overlay->setHeight(20.0f);
        app.registerLocalTransient(std::move(overlay), TransientOptions{.dismissOnOutsidePointer = true});
        app.updateLayout();
    };

    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 11));
    EXPECT_EQ(app.getTransientController().size(), 1u);
    ASSERT_TRUE(app.sendPointerUp(80.0f, 60.0f, 0, PointerSource::Touch, 11));
    EXPECT_EQ(app.getTransientController().size(), 0u);

    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 12));
    app.sendPointerMove(81.0f, 61.0f, PointerSource::Touch, 12);
    ASSERT_TRUE(app.sendPointerUp(81.0f, 61.0f, 0, PointerSource::Touch, 12));
    EXPECT_EQ(app.getTransientController().size(), 0u);

    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 13));
    app.sendPointerMove(88.0f, 60.0f, PointerSource::Touch, 13);
    ASSERT_TRUE(app.sendPointerUp(88.0f, 60.0f, 0, PointerSource::Touch, 13));
    EXPECT_EQ(app.getTransientController().size(), 1u);

    app.clearTransients();
    addDismissable();
    ASSERT_TRUE(app.sendPointerDown(80.0f, 60.0f, 0, PointerSource::Touch, 14));
    ASSERT_TRUE(app.sendPointerCancel(80.0f, 60.0f, PointerSource::Touch, 14));
    EXPECT_EQ(app.getTransientController().size(), 1u);
}

TEST(LclUiTest, TransientControllerDoesNotDismissForScrollViewTouchGesture) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Overlay scroll gesture");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    auto scroll = std::make_unique<ScrollView>();
    scroll->setWidth(100.0f);
    scroll->setHeight(80.0f);
    auto content = std::make_unique<Container>();
    content->setWidth(100.0f);
    content->setHeight(240.0f);
    scroll->setContent(std::move(content));
    root->addChild(std::move(scroll));
    app.setRootWidget(std::move(root));

    auto overlay = std::make_unique<PointerProbeWidget>();
    overlay->setWidth(10.0f);
    overlay->setHeight(10.0f);
    app.registerLocalTransient(std::move(overlay), TransientOptions{.dismissOnOutsidePointer = true});
    app.updateLayout();

    app.sendPointerDown(50.0f, 40.0f, 0, PointerSource::Touch, 21);
    ASSERT_TRUE(app.sendPointerMove(50.0f, 52.0f, PointerSource::Touch, 21));
    ASSERT_TRUE(app.sendPointerUp(50.0f, 52.0f, 0, PointerSource::Touch, 21));
    EXPECT_EQ(app.getTransientController().size(), 1u);
}

TEST(LclUiTest, TransientControllerUsesTheSameStableLifecycleForSurfaceEntries) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Surface transient lifecycle");
    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    app.setRootWidget(std::move(root));

    int destroyCount = 0;
    int dismissCount = 0;
    const TransientHandle handle = app.registerSurfaceTransient(
        7, [&] { ++destroyCount; }, TransientOptions{
            .dismissOnOutsidePointer = true,
            .onDismiss = [&] { ++dismissCount; },
        });
    ASSERT_NE(handle, 0u);
    EXPECT_EQ(app.getTransientController().size(), 1u);

    app.sendPointerDown(40.0f, 30.0f);
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(destroyCount, 1);
    EXPECT_EQ(dismissCount, 1);

    const TransientHandle explicitHandle = app.registerSurfaceTransient(
        8, [&] { ++destroyCount; });
    ASSERT_TRUE(app.removeTransient(explicitHandle));
    EXPECT_EQ(destroyCount, 2);
    EXPECT_EQ(dismissCount, 1);
}

class MenuTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                             popupSockets), 0);
        app = std::make_unique<WindowApp>(
            std::make_unique<RecordingCanvas>(), 300, 220, "Menu test");
        auto root = std::make_unique<Container>();
        root->setWidth(300.0f);
        root->setHeight(220.0f);
        auto anchorWidget = std::make_unique<Button>("Menu anchor");
        anchorWidget->setPositionType(layout::PositionType::Absolute);
        anchorWidget->setPosition(layout::Edge::Left, 20.0f);
        anchorWidget->setPosition(layout::Edge::Top, 20.0f);
        anchorWidget->setWidth(100.0f);
        anchorWidget->setHeight(32.0f);
        anchor = anchorWidget.get();
        root->addChild(std::move(anchorWidget));
        app->setRootWidget(std::move(root));
        app->updateLayout();
        menu = std::make_unique<Menu>(
            *app, [] { return std::make_unique<RecordingCanvas>(); },
            [this](WindowApp& popup) {
                popupWindow = &popup;
                popupSurfaceId = popup.getSurfaceId();
                popup.setExternalIpcSocket(popupSockets[0]);
                return true;
            });
    }

    void TearDown() override {
        menu.reset();
        app.reset();
        close(popupSockets[0]);
        close(popupSockets[1]);
    }

    PopoverOpenResult open(std::vector<MenuItem> items,
                           std::function<void()> onDismissed = {}) {
        app->getDispatcher().setFocus(anchor);
        auto result = menu->show(
            *anchor, std::move(items),
            MenuOptions{.width = 180.0f,
                        .itemHeight = 36.0f,
                        .onDismissed = std::move(onDismissed)});
        app->tick();
        return result;
    }

    std::vector<Button*> popupItems() const {
        std::vector<Button*> result;
        if (!popupWindow || !popupWindow->getRootWidget()) return result;
        const auto& roots = popupWindow->getRootWidget()->getChildren();
        if (roots.empty()) return result;
        for (const auto& child : roots.front()->getChildren()) {
            if (auto* button = dynamic_cast<Button*>(child.get())) {
                result.push_back(button);
            }
        }
        return result;
    }

    void sendKey(lcl::platform::PhysicalKey key) {
        lcl::protocol::LCLMsgInputEvent input{};
        input.surfaceId = popupSurfaceId;
        input.type = 1;
        input.key = static_cast<uint32_t>(key);
        input.pressed = 1;
        lcl::protocol::LCLHeader header{};
        header.opcode = lcl::protocol::LCLOpcode::InputEvent;
        header.payloadSize = sizeof(input);
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(
            popupSockets[1], header, &input));
        app->tick();
    }

    void sendPointer(PointerEventType type, float x, float y,
                     PointerSource source = PointerSource::Mouse) {
        lcl::protocol::LCLMsgInputEvent input{};
        input.surfaceId = popupSurfaceId;
        input.type = type == PointerEventType::Move ? 3 : 4;
        input.x = x;
        input.y = y;
        input.key = 0;
        input.pressed = type == PointerEventType::Down ? 1 : 0;
        input.source = static_cast<uint8_t>(
            source == PointerSource::Touch
                ? lcl::protocol::LCLPointerSource::Touch
                : lcl::protocol::LCLPointerSource::Mouse);
        lcl::protocol::LCLHeader header{};
        header.opcode = lcl::protocol::LCLOpcode::InputEvent;
        header.payloadSize = sizeof(input);
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(
            popupSockets[1], header, &input));
        app->tick();
    }

    int popupSockets[2]{-1, -1};
    std::unique_ptr<WindowApp> app;
    std::unique_ptr<Menu> menu;
    Button* anchor{nullptr};
    WindowApp* popupWindow{nullptr};
    uint32_t popupSurfaceId{0};
};

TEST_F(MenuTest, OpeningAndArrowNavigationSkipDisabledItemsAndWrap) {
    const auto opened = open({
        MenuItem{"Disabled first", false, {}},
        MenuItem{"First", true, {}},
        MenuItem{"Disabled middle", false, {}},
        MenuItem{"Last", true, {}},
    });
    ASSERT_TRUE(opened);
    const auto items = popupItems();
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(app->getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), items[1]);

    sendKey(lcl::platform::PhysicalKey::ArrowDown);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), items[3]);
    sendKey(lcl::platform::PhysicalKey::ArrowDown);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), items[1]);
    sendKey(lcl::platform::PhysicalKey::ArrowUp);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), items[3]);
    sendKey(lcl::platform::PhysicalKey::ArrowUp);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), items[1]);
}

TEST_F(MenuTest, EnterAndSpaceActivateThenCloseAndRestoreAnchor) {
    int enterActivations = 0;
    auto opened = open({MenuItem{"Enter", true,
                                [&] { ++enterActivations; }}});
    ASSERT_TRUE(opened);
    sendKey(lcl::platform::PhysicalKey::Enter);
    EXPECT_EQ(enterActivations, 1);
    EXPECT_FALSE(menu->isOpen(opened.handle));
    EXPECT_EQ(app->getDispatcher().getFocusedWidget(), anchor);

    int spaceActivations = 0;
    opened = open({MenuItem{"Space", true,
                           [&] { ++spaceActivations; }}});
    ASSERT_TRUE(opened);
    sendKey(lcl::platform::PhysicalKey::Space);
    EXPECT_EQ(spaceActivations, 1);
    EXPECT_FALSE(menu->isOpen(opened.handle));
    EXPECT_EQ(app->getDispatcher().getFocusedWidget(), anchor);
}

TEST_F(MenuTest, MouseAndValidatedTouchActivateButDragCancelAndDisabledDoNot) {
    int activations = 0;
    auto opened = open({MenuItem{"Mouse", true, [&] { ++activations; }}});
    ASSERT_TRUE(opened);
    auto items = popupItems();
    ASSERT_EQ(items.size(), 1u);
    graphics::RectF bounds = items[0]->getAbsoluteBounds();
    sendPointer(PointerEventType::Down, bounds.x + 8.0f, bounds.y + 8.0f);
    sendPointer(PointerEventType::Up, bounds.x + 8.0f, bounds.y + 8.0f);
    EXPECT_EQ(activations, 1);
    EXPECT_FALSE(menu->isOpen(opened.handle));

    opened = open({MenuItem{"Touch", true, [&] { ++activations; }}});
    ASSERT_TRUE(opened);
    items = popupItems();
    bounds = items[0]->getAbsoluteBounds();
    sendPointer(PointerEventType::Down, bounds.x + 8.0f, bounds.y + 8.0f,
                PointerSource::Touch);
    sendPointer(PointerEventType::Up, bounds.x + 8.0f, bounds.y + 8.0f,
                PointerSource::Touch);
    EXPECT_EQ(activations, 2);
    EXPECT_FALSE(menu->isOpen(opened.handle));

    opened = open({MenuItem{"Gesture", true, [&] { ++activations; }},
                   MenuItem{"Disabled", false, [&] { ++activations; }}});
    ASSERT_TRUE(opened);
    items = popupItems();
    ASSERT_EQ(items.size(), 2u);
    bounds = items[0]->getAbsoluteBounds();
    sendPointer(PointerEventType::Down, bounds.x + 8.0f, bounds.y + 8.0f,
                PointerSource::Touch);
    sendPointer(PointerEventType::Move, bounds.x + 24.0f, bounds.y + 8.0f,
                PointerSource::Touch);
    sendPointer(PointerEventType::Up, bounds.x + 24.0f, bounds.y + 8.0f,
                PointerSource::Touch);
    EXPECT_EQ(activations, 2);
    EXPECT_TRUE(menu->isOpen(opened.handle));

    popupWindow->sendPointerDown(bounds.x + 8.0f, bounds.y + 8.0f, 0,
                                 PointerSource::Touch, 9);
    popupWindow->sendPointerCancel(bounds.x + 8.0f, bounds.y + 8.0f,
                                   PointerSource::Touch, 9);
    EXPECT_EQ(activations, 2);
    EXPECT_TRUE(menu->isOpen(opened.handle));

    const graphics::RectF disabledBounds = items[1]->getAbsoluteBounds();
    sendPointer(PointerEventType::Down, disabledBounds.x + 8.0f,
                disabledBounds.y + 8.0f);
    sendPointer(PointerEventType::Up, disabledBounds.x + 8.0f,
                disabledBounds.y + 8.0f);
    EXPECT_EQ(activations, 2);
    EXPECT_TRUE(menu->isOpen(opened.handle));
}

TEST_F(MenuTest, EscapeAndOutsideDismissCloseWithoutActivation) {
    int activations = 0;
    auto opened = open({MenuItem{"Action", true, [&] { ++activations; }}});
    ASSERT_TRUE(opened);
    sendKey(lcl::platform::PhysicalKey::Escape);
    EXPECT_EQ(activations, 0);
    EXPECT_FALSE(menu->isOpen(opened.handle));
    EXPECT_EQ(app->getDispatcher().getFocusedWidget(), anchor);

    int dismissals = 0;
    opened = open({MenuItem{"Action", true, [&] { ++activations; }}},
                  [&] { ++dismissals; });
    ASSERT_TRUE(opened);
    EXPECT_TRUE(app->sendPointerDown(260.0f, 180.0f));
    EXPECT_EQ(dismissals, 1);
    EXPECT_EQ(activations, 0);
    EXPECT_FALSE(menu->isOpen(opened.handle));
    EXPECT_EQ(app->getDispatcher().getFocusedWidget(), anchor);
}

TEST_F(MenuTest, CallbackMayCloseItsOwnTransientAndZeroEnabledItemsStaySafe) {
    int activations = 0;
    auto handleSlot = std::make_shared<TransientHandle>(0);
    auto opened = open({MenuItem{
        "Self close", true,
        [&, handleSlot] {
            ++activations;
            menu->close(*handleSlot);
        }}});
    ASSERT_TRUE(opened);
    *handleSlot = opened.handle;
    sendKey(lcl::platform::PhysicalKey::Enter);
    EXPECT_EQ(activations, 1);
    EXPECT_FALSE(menu->isOpen(opened.handle));
    EXPECT_EQ(app->getDispatcher().getFocusedWidget(), anchor);

    opened = open({MenuItem{"Disabled A", false, [&] { ++activations; }},
                   MenuItem{"Disabled B", false, [&] { ++activations; }}});
    ASSERT_TRUE(opened);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), nullptr);
    sendKey(lcl::platform::PhysicalKey::ArrowDown);
    sendKey(lcl::platform::PhysicalKey::ArrowUp);
    EXPECT_EQ(activations, 1);
    EXPECT_TRUE(menu->isOpen(opened.handle));
    EXPECT_TRUE(menu->close(opened.handle));
    EXPECT_EQ(app->getDispatcher().getFocusedWidget(), anchor);
}

TEST(LclUiTest, PopoverPlacesBelowLeftAndAlwaysUsesPopupSurface) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* mainCanvas = canvas.get();
    WindowApp app(std::move(canvas), 300, 200, "Popover presentation choice");
    app.setAppId("org.lcl.test.popover");

    auto root = std::make_unique<Container>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    Container* rootPtr = root.get();
    auto localAnchor = std::make_unique<Widget>();
    localAnchor->setPositionType(layout::PositionType::Absolute);
    localAnchor->setPosition(layout::Edge::Left, 20.0f);
    localAnchor->setPosition(layout::Edge::Top, 30.0f);
    localAnchor->setWidth(50.0f);
    localAnchor->setHeight(20.0f);
    Widget* localAnchorPtr = localAnchor.get();
    auto edgeAnchor = std::make_unique<Widget>();
    edgeAnchor->setPositionType(layout::PositionType::Absolute);
    edgeAnchor->setPosition(layout::Edge::Left, 260.0f);
    edgeAnchor->setPosition(layout::Edge::Top, 160.0f);
    edgeAnchor->setWidth(30.0f);
    edgeAnchor->setHeight(20.0f);
    Widget* edgeAnchorPtr = edgeAnchor.get();
    root->addChild(std::move(localAnchor));
    root->addChild(std::move(edgeAnchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();
    ASSERT_TRUE(app.renderFrame());

    uint32_t popupSurfaceId = 0;
    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popupSurfaceId = popup.getSurfaceId();
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });

    const auto insideWindow = popover.show(
        *localAnchorPtr, std::make_unique<Button>("Inside-window content"),
        PopoverOptions{.width = 100.0f, .height = 60.0f});
    ASSERT_TRUE(insideWindow);
    EXPECT_EQ(insideWindow.presentation, PopoverPresentation::PopupSurface);
    EXPECT_FLOAT_EQ(insideWindow.geometry.x, 20.0f);
    EXPECT_FLOAT_EQ(insideWindow.geometry.y, 50.0f);
    EXPECT_TRUE(popover.isOpen(insideWindow.handle));
    EXPECT_NE(popupSurfaceId, 0u);
    EXPECT_EQ(app.hostedSurfaceCount(), 1u);
    EXPECT_TRUE(popover.close(insideWindow.handle));
    EXPECT_EQ(app.hostedSurfaceCount(), 0u);

    const auto popup = popover.show(
        *edgeAnchorPtr, std::make_unique<Button>("Popup content"),
        PopoverOptions{.width = 100.0f, .height = 60.0f});
    ASSERT_TRUE(popup);
    EXPECT_EQ(popup.presentation, PopoverPresentation::PopupSurface);
    EXPECT_FLOAT_EQ(popup.geometry.x, 260.0f);
    EXPECT_FLOAT_EQ(popup.geometry.y, 180.0f);
    EXPECT_NE(popupSurfaceId, 0u);
    EXPECT_NE(popupSurfaceId, app.getSurfaceId());
    EXPECT_TRUE(popover.isOpen(popup.handle));
    EXPECT_EQ(app.hostedSurfaceCount(), 1u);

    // Match the runtime failure ordering: the popup is idle while the main
    // WindowApp renders and therefore leaves its own backend current.
    rootPtr->invalidatePaint();
    ASSERT_TRUE(app.renderFrame());
    EXPECT_TRUE(popover.close(popup.handle));
    EXPECT_FALSE(popover.isOpen(popup.handle));
    EXPECT_EQ(app.hostedSurfaceCount(), 0u);

    const int mainFramesBeforePostPopupRender = mainCanvas->beginCount;
    rootPtr->invalidatePaint();
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(mainCanvas->beginCount, mainFramesBeforePostPopupRender + 1);

    const auto ownerBoundPopup = popover.show(
        *edgeAnchorPtr, std::make_unique<Button>("Owner-bound popup"),
        PopoverOptions{.width = 100.0f, .height = 60.0f});
    ASSERT_TRUE(ownerBoundPopup);
    rootPtr->removeChild(edgeAnchorPtr);
    app.tick();
    EXPECT_FALSE(popover.isOpen(ownerBoundPopup.handle));
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(app.hostedSurfaceCount(), 0u);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopoverHandsFocusToScopedContentAndRestoresAnchor) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    WindowApp app(std::make_unique<RecordingCanvas>(), 300, 200,
                  "Popover focus handoff");
    auto root = std::make_unique<Container>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    auto anchor = std::make_unique<Button>("Anchor");
    anchor->setWidth(80.0f);
    anchor->setHeight(30.0f);
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();
    app.getDispatcher().setFocus(anchorPtr);

    WindowApp* popupWindow = nullptr;
    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popupWindow = &popup;
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    auto popupContent = std::make_unique<Container>();
    auto disabled = std::make_unique<Button>("Disabled");
    Button* disabledPtr = disabled.get();
    disabled->setEnabled(false);
    auto first = std::make_unique<TextField>();
    TextField* firstPtr = first.get();
    auto last = std::make_unique<Button>("Last");
    Button* lastPtr = last.get();
    popupContent->addChild(std::move(disabled));
    popupContent->addChild(std::move(first));
    popupContent->addChild(std::move(last));

    const auto opened = popover.show(
        *anchorPtr, std::move(popupContent),
        PopoverOptions{.width = 160.0f, .height = 120.0f});
    ASSERT_TRUE(opened);
    ASSERT_NE(popupWindow, nullptr);
    ASSERT_NE(popupWindow->getRootWidget(), nullptr);
    EXPECT_TRUE(popupWindow->getRootWidget()->isFocusScope());
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), firstPtr);
    EXPECT_NE(popupWindow->getDispatcher().getFocusedWidget(), disabledPtr);

    EXPECT_TRUE(popupWindow->sendKeyDown(
        static_cast<int>(lcl::platform::PhysicalKey::Tab)));
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), lastPtr);
    EXPECT_TRUE(popupWindow->sendKeyDown(
        static_cast<int>(lcl::platform::PhysicalKey::Tab)));
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), firstPtr);
    EXPECT_TRUE(popupWindow->sendKeyDown(
        static_cast<int>(lcl::platform::PhysicalKey::Tab), 0,
        lcl::platform::kModShift));
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), lastPtr);

    EXPECT_TRUE(popover.close(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), anchorPtr);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopoverWithoutFocusableContentKeepsPopupFocusEmpty) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    WindowApp app(std::make_unique<RecordingCanvas>(), 300, 200,
                  "Popover empty focus");
    auto root = std::make_unique<Container>();
    auto anchor = std::make_unique<Button>("Anchor");
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();
    app.getDispatcher().setFocus(anchorPtr);

    WindowApp* popupWindow = nullptr;
    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popupWindow = &popup;
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    const auto opened = popover.show(
        *anchorPtr, std::make_unique<Text>("No focus target"),
        PopoverOptions{.width = 140.0f, .height = 70.0f});
    ASSERT_TRUE(opened);
    ASSERT_NE(popupWindow, nullptr);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_TRUE(popover.close(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), anchorPtr);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopoverDoesNotRestoreAnIneligibleAnchor) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    WindowApp app(std::make_unique<RecordingCanvas>(), 300, 200,
                  "Popover ineligible restore target");
    auto root = std::make_unique<Container>();
    auto anchor = std::make_unique<Button>("Anchor");
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();
    app.getDispatcher().setFocus(anchorPtr);

    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    const auto opened = popover.show(
        *anchorPtr, std::make_unique<Button>("Popup child"),
        PopoverOptions{.width = 140.0f, .height = 70.0f});
    ASSERT_TRUE(opened);
    anchorPtr->setInteractionEnabled(false);
    EXPECT_TRUE(popover.close(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopupPopoverContentReceivesInputAndOutsideMouseDismisses) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 300, 200, "Popup popover input");
    auto root = std::make_unique<Container>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    auto background = std::make_unique<PointerProbeWidget>();
    background->setWidth(300.0f);
    background->setHeight(200.0f);
    root->addChild(std::move(background));
    auto anchor = std::make_unique<Button>("Anchor");
    anchor->setPositionType(layout::PositionType::Absolute);
    anchor->setPosition(layout::Edge::Left, 20.0f);
    anchor->setPosition(layout::Edge::Top, 20.0f);
    anchor->setWidth(60.0f);
    anchor->setHeight(20.0f);
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();
    app.getDispatcher().setFocus(anchorPtr);

    int insideClicks = 0;
    int dismissals = 0;
    uint32_t popupSurfaceId = 0;
    auto inside = std::make_unique<Button>("Inside");
    inside->setOnClick([&] { ++insideClicks; });
    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popupSurfaceId = popup.getSurfaceId();
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    const auto opened = popover.show(
        *anchorPtr, std::move(inside),
        PopoverOptions{
            .width = 120.0f,
            .height = 70.0f,
            .onDismissed = [&] { ++dismissals; },
        });
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened.presentation, PopoverPresentation::PopupSurface);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    app.tick();

    const auto sendPopupPointer = [&](bool pressed) {
        lcl::protocol::LCLMsgInputEvent input{};
        input.surfaceId = popupSurfaceId;
        input.type = 4;
        input.x = 20.0f;
        input.y = 20.0f;
        input.pressed = pressed ? 1 : 0;
        input.source = static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Mouse);
        lcl::protocol::LCLHeader header{};
        header.opcode = lcl::protocol::LCLOpcode::InputEvent;
        header.payloadSize = sizeof(input);
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(popupSockets[1], header, &input));
        app.tick();
    };
    sendPopupPointer(true);
    sendPopupPointer(false);
    EXPECT_EQ(insideClicks, 1);
    EXPECT_TRUE(popover.isOpen(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);

    EXPECT_TRUE(app.sendPointerDown(250.0f, 160.0f));
    EXPECT_FALSE(popover.isOpen(opened.handle));
    EXPECT_EQ(dismissals, 1);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), anchorPtr);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopoverTouchDismissRequiresValidatedOutsideTap) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 300, 200, "Popover touch dismissal");
    auto root = std::make_unique<Container>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    auto background = std::make_unique<PointerProbeWidget>();
    background->setWidth(300.0f);
    background->setHeight(200.0f);
    root->addChild(std::move(background));
    auto anchor = std::make_unique<Button>("Anchor");
    anchor->setPositionType(layout::PositionType::Absolute);
    anchor->setPosition(layout::Edge::Left, 20.0f);
    anchor->setPosition(layout::Edge::Top, 20.0f);
    anchor->setWidth(60.0f);
    anchor->setHeight(20.0f);
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();

    WindowApp* popupWindow = nullptr;
    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popupWindow = &popup;
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    const auto open = [&] {
        app.getDispatcher().setFocus(anchorPtr);
        const auto result = popover.show(
            *anchorPtr, std::make_unique<Button>("Touch content"),
            PopoverOptions{.width = 120.0f, .height = 70.0f});
        app.updateLayout();
        return result;
    };

    auto opened = open();
    ASSERT_TRUE(opened);
    ASSERT_NE(popupWindow, nullptr);
    Widget* popupFocus = popupWindow->getDispatcher().getFocusedWidget();
    ASSERT_NE(popupFocus, nullptr);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    app.sendPointerDown(250.0f, 160.0f, 0, PointerSource::Touch, 41);
    EXPECT_TRUE(popover.isOpen(opened.handle));
    app.sendPointerUp(250.0f, 160.0f, 0, PointerSource::Touch, 41);
    EXPECT_FALSE(popover.isOpen(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), anchorPtr);

    opened = open();
    popupFocus = popupWindow->getDispatcher().getFocusedWidget();
    ASSERT_NE(popupFocus, nullptr);
    app.sendPointerDown(250.0f, 150.0f, 0, PointerSource::Touch, 42);
    app.sendPointerMove(250.0f, 164.0f, PointerSource::Touch, 42);
    app.sendPointerUp(250.0f, 164.0f, 0, PointerSource::Touch, 42);
    EXPECT_TRUE(popover.isOpen(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), popupFocus);

    app.sendPointerDown(250.0f, 150.0f, 0, PointerSource::Touch, 43);
    app.sendPointerCancel(250.0f, 150.0f, PointerSource::Touch, 43);
    EXPECT_TRUE(popover.isOpen(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    EXPECT_EQ(popupWindow->getDispatcher().getFocusedWidget(), popupFocus);
    EXPECT_TRUE(popover.close(opened.handle));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), anchorPtr);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopoverAnchorDestructionPrunesItsTransient) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 300, 200, "Popover anchor lifetime");
    auto root = std::make_unique<Container>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    Container* rootPtr = root.get();
    auto anchor = std::make_unique<Button>("Anchor");
    anchor->setWidth(60.0f);
    anchor->setHeight(20.0f);
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();
    app.getDispatcher().setFocus(anchorPtr);

    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    const auto opened = popover.show(
        *anchorPtr, std::make_unique<Button>("Owned content"),
        PopoverOptions{.width = 120.0f, .height = 70.0f});
    ASSERT_TRUE(opened);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    rootPtr->removeChild(anchorPtr);
    app.tick();
    EXPECT_FALSE(popover.isOpen(opened.handle));
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(app.hostedSurfaceCount(), 0u);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopupPopoverUsesWidgetInputAndDropsStaleHandleOnSurfaceDestroy) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 300, 200, "Popup popover input");
    app.setAppId("org.lcl.test.popover-input");
    auto root = std::make_unique<Container>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    auto anchor = std::make_unique<Button>("Anchor");
    anchor->setPositionType(layout::PositionType::Absolute);
    anchor->setPosition(layout::Edge::Left, 260.0f);
    anchor->setPosition(layout::Edge::Top, 160.0f);
    anchor->setWidth(30.0f);
    anchor->setHeight(20.0f);
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app.setRootWidget(std::move(root));
    app.updateLayout();
    app.getDispatcher().setFocus(anchorPtr);

    uint32_t popupSurfaceId = 0;
    int insideClicks = 0;
    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popupSurfaceId = popup.getSurfaceId();
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    auto button = std::make_unique<Button>("Popup inside");
    button->setOnClick([&] { ++insideClicks; });
    const auto opened = popover.show(
        *anchorPtr, std::move(button),
        PopoverOptions{.width = 120.0f, .height = 70.0f});
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened.presentation, PopoverPresentation::PopupSurface);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), nullptr);
    app.tick();

    const auto sendPointer = [&](bool pressed) {
        lcl::protocol::LCLMsgInputEvent input{};
        input.surfaceId = popupSurfaceId;
        input.type = 4;
        input.x = 20.0f;
        input.y = 20.0f;
        input.key = 0;
        input.pressed = pressed ? 1 : 0;
        input.source = static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Mouse);
        lcl::protocol::LCLHeader header{};
        header.opcode = lcl::protocol::LCLOpcode::InputEvent;
        header.payloadSize = sizeof(input);
        ASSERT_TRUE(lcl::protocol::sendMsgWithFd(popupSockets[1], header, &input));
        app.tick();
    };
    sendPointer(true);
    sendPointer(false);
    EXPECT_EQ(insideClicks, 1);
    EXPECT_TRUE(popover.isOpen(opened.handle));

    lcl::protocol::LCLMsgSurfaceDestroy destroy{};
    destroy.surfaceId = popupSurfaceId;
    lcl::protocol::LCLHeader destroyHeader{};
    destroyHeader.opcode = lcl::protocol::LCLOpcode::SurfaceDestroy;
    destroyHeader.payloadSize = sizeof(destroy);
    ASSERT_TRUE(lcl::protocol::sendMsgWithFd(
        popupSockets[1], destroyHeader, &destroy));
    app.tick();
    EXPECT_FALSE(popover.isOpen(opened.handle));
    EXPECT_EQ(app.getTransientController().size(), 0u);
    EXPECT_EQ(app.hostedSurfaceCount(), 0u);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), anchorPtr);

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, PopupPopoverUsesTheSameOutsideDismissPolicy) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 300, 200, "Popup popover dismissal");
    auto root = std::make_unique<PointerProbeWidget>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    app.setRootWidget(std::move(root));
    app.updateLayout();

    int dismissals = 0;
    Popover popover(
        app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    const auto openPopup = [&] {
        return popover.show(
            graphics::RectF{260.0f, 160.0f, 30.0f, 20.0f},
            std::make_unique<Button>("Popup"),
            PopoverOptions{
                .width = 120.0f,
                .height = 70.0f,
                .onDismissed = [&] { ++dismissals; },
            });
    };

    auto opened = openPopup();
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened.presentation, PopoverPresentation::PopupSurface);
    EXPECT_TRUE(app.sendPointerDown(10.0f, 10.0f));
    EXPECT_FALSE(popover.isOpen(opened.handle));
    EXPECT_EQ(dismissals, 1);
    EXPECT_EQ(app.hostedSurfaceCount(), 0u);

    opened = openPopup();
    ASSERT_TRUE(opened);
    app.sendPointerDown(10.0f, 10.0f, 0, PointerSource::Touch, 51);
    EXPECT_TRUE(popover.isOpen(opened.handle));
    app.sendPointerUp(10.0f, 10.0f, 0, PointerSource::Touch, 51);
    EXPECT_FALSE(popover.isOpen(opened.handle));
    EXPECT_EQ(dismissals, 2);

    opened = openPopup();
    ASSERT_TRUE(opened);
    app.sendPointerDown(10.0f, 10.0f, 0, PointerSource::Touch, 52);
    app.sendPointerMove(24.0f, 10.0f, PointerSource::Touch, 52);
    app.sendPointerUp(24.0f, 10.0f, 0, PointerSource::Touch, 52);
    EXPECT_TRUE(popover.isOpen(opened.handle));
    app.sendPointerDown(10.0f, 10.0f, 0, PointerSource::Touch, 53);
    app.sendPointerCancel(10.0f, 10.0f, PointerSource::Touch, 53);
    EXPECT_TRUE(popover.isOpen(opened.handle));
    EXPECT_TRUE(popover.close(opened.handle));

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, WindowDestructionCleansPopupPopoverAndPopoverHandleSafely) {
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         popupSockets), 0);
    auto app = std::make_unique<WindowApp>(
        std::make_unique<RecordingCanvas>(), 300, 200,
        "Popover window lifetime");
    auto root = std::make_unique<Container>();
    root->setWidth(300.0f);
    root->setHeight(200.0f);
    auto anchor = std::make_unique<Button>("Anchor");
    anchor->setWidth(80.0f);
    anchor->setHeight(30.0f);
    Button* anchorPtr = anchor.get();
    root->addChild(std::move(anchor));
    app->setRootWidget(std::move(root));
    app->updateLayout();
    app->getDispatcher().setFocus(anchorPtr);

    int contentDestructions = 0;
    auto popover = std::make_unique<Popover>(
        *app, [] { return std::make_unique<RecordingCanvas>(); },
        [&](WindowApp& popup) {
            popup.setExternalIpcSocket(popupSockets[0]);
            return true;
        });
    auto content = std::make_unique<Widget>();
    content->setDestructionCallback([&] { ++contentDestructions; });
    const auto opened = popover->show(
        *anchorPtr, std::move(content),
        PopoverOptions{.width = 120.0f, .height = 70.0f});
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened.presentation, PopoverPresentation::PopupSurface);

    app.reset();
    EXPECT_EQ(contentDestructions, 1);
    EXPECT_FALSE(popover->isOpen(opened.handle));
    popover.reset();

    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(LclUiTest, WindowAppSurfaceTransientRemovalRequestsPopupDestroy) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Popup teardown request");
    app.setExternalIpcSocket(sockets[0]);

    const TransientHandle handle = app.registerSurfaceTransient(7);
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(app.removeTransient(handle));

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, lcl::protocol::LCLOpcode::RequestSurfaceClose);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgRequestSurfaceClose));
    EXPECT_EQ(reinterpret_cast<const lcl::protocol::LCLMsgRequestSurfaceClose*>(
                  payload.data())->surfaceId,
              7u);
    if (receivedFd >= 0) close(receivedFd);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, WindowAppGatesLayoutToLayoutAffectingMutations) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Layout dirty gating");

    auto root = std::make_unique<CountingLayoutContainer>();
    CountingLayoutContainer* rootPointer = root.get();
    root->setWidth(64.0f);
    root->setHeight(48.0f);
    app.setRootWidget(std::move(root));

    EXPECT_TRUE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    EXPECT_EQ(rootPointer->syncLayoutCount, 1);

    rootPointer->invalidatePaint();
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(rootPointer->syncLayoutCount, 1);

    rootPointer->setTranslationY(3.0f);
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(rootPointer->syncLayoutCount, 1);

    rootPointer->setWidth(60.0f);
    EXPECT_TRUE(rootPointer->isLayoutDirty());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_FALSE(rootPointer->isLayoutDirty());
    EXPECT_EQ(rootPointer->syncLayoutCount, 2);
}

TEST(LclUiTest, WindowAppCreatesPopupWithParentLocalGeometry) {
    const std::string socketPath = "/tmp/lcl-ui-popup-create-" +
        std::to_string(getpid()) + ".sock";
    unlink(socketPath.c_str());

    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);

    WindowApp popup(lcl::render::makeDisplayListCanvas(), 120, 70, "Popup");
    popup.setAppId("org.lcl.test.popup");
    popup.setSurfaceId(2);
    popup.configurePopupSurface(1, lcl::protocol::LCLPopupRole::Transient, 250, -8);
    ASSERT_TRUE(popup.setWindowCornerStyle(10.0f, 2.0f));
    ASSERT_TRUE(popup.connectCompositor(socketPath));

    const int peer = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    ASSERT_GE(peer, 0);
    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(peer, header, payload, receivedFd));
    ASSERT_EQ(header.opcode, lcl::protocol::LCLOpcode::PopupSurfaceCreate);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgPopupSurfaceCreate));
    const auto* request = reinterpret_cast<const lcl::protocol::LCLMsgPopupSurfaceCreate*>(payload.data());
    EXPECT_EQ(request->surfaceId, 2u);
    EXPECT_EQ(request->parentSurfaceId, 1u);
    EXPECT_EQ(request->role, lcl::protocol::LCLPopupRole::Transient);
    EXPECT_EQ(request->x, 250);
    EXPECT_EQ(request->y, -8);
    EXPECT_EQ(request->width, 120u);
    EXPECT_EQ(request->height, 70u);
    if (receivedFd >= 0) close(receivedFd);

    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(peer, header, payload, receivedFd));
    ASSERT_EQ(header.opcode, lcl::protocol::LCLOpcode::SetWindowCornerStyle);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgSetWindowCornerStyle));
    const auto* cornerStyle =
        reinterpret_cast<const lcl::protocol::LCLMsgSetWindowCornerStyle*>(payload.data());
    EXPECT_EQ(cornerStyle->surfaceId, 2u);
    EXPECT_FLOAT_EQ(cornerStyle->radius, 10.0f);
    EXPECT_FLOAT_EQ(cornerStyle->roundness, 2.0f);
    if (receivedFd >= 0) close(receivedFd);

    close(peer);
    close(listener);
    unlink(socketPath.c_str());
}

TEST(LclUiTest, ResizeFirstFramePaintsPostLayoutRootExtent) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 100, 100, "Resize damage");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(100.0f);
    root->setAlignItems(layout::Align::Center);
    root->setJustifyContent(layout::Justify::Center);
    auto child = std::make_unique<Button>("Moved");
    child->setWidth(40.0f);
    child->setHeight(24.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());

    recorded->roundedRects.clear();
    recorded->texts.clear();
    recorded->textPositions.clear();
    app.resize(400, 100);
    ASSERT_TRUE(app.renderFrame());

    ASSERT_FALSE(recorded->roundedRects.empty());
    EXPECT_FLOAT_EQ(app.getRootWidget()->getBounds().width, 400.0f);
    EXPECT_FLOAT_EQ(app.getRootWidget()->getBounds().height, 100.0f);
    EXPECT_GT(recorded->roundedRects.back().x, 100.0f);
    ASSERT_EQ(recorded->texts.size(), 1u);
    EXPECT_EQ(recorded->texts.front(), "Moved");

}

TEST(LclUiTest, WindowContentRootFollowsBothResizeAxes) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 80, "Two-axis resize");

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(80.0f);
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.resize(260, 190);
    ASSERT_TRUE(app.renderFrame());

    ASSERT_NE(app.getRootWidget(), nullptr);
    EXPECT_FLOAT_EQ(app.getRootWidget()->getBounds().width, 260.0f);
    EXPECT_FLOAT_EQ(app.getRootWidget()->getBounds().height, 190.0f);
}

TEST(LclUiTest, ImplicitTransactionInterpolatesTransformOpacityAndReflowLayout) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 320, 200, "Motion transaction");
    auto root = std::make_unique<Container>();
    root->setWidth(320.0f);
    root->setHeight(200.0f);
    auto child = std::make_unique<Button>("Animated");
    Button* pointer = child.get();
    child->setWidth(100.0f);
    child->setHeight(40.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.animate(Motion::tween(1.0f, Easing::linear()), {LayoutMode::Reflow}, [&] {
        pointer->setWidth(200.0f);
        pointer->setOpacity(0.0f);
        pointer->setTranslationX(20.0f);
    });
    EXPECT_TRUE(app.advanceAnimations(0.5f));
    EXPECT_NEAR(pointer->getAbsoluteBounds().width, 150.0f, 0.01f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.5f, 0.01f);
    EXPECT_NEAR(pointer->getPresentationState().translationX, 10.0f, 0.01f);
    EXPECT_EQ(app.getDispatcher().hitTest(app.getRootWidget(), 140.0f, 20.0f), pointer);
}

TEST(LclUiTest, ZeroOpacityChildStillBubblesTapToParent) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 120, 80, "Transparent hit target");
    auto tile = std::make_unique<Container>();
    tile->setWidth(60.0f);
    tile->setHeight(60.0f);
    auto icon = std::make_unique<Container>();
    icon->setWidth(60.0f);
    icon->setHeight(60.0f);
    icon->setOpacity(0.0f);
    tile->addChild(std::move(icon));
    int taps = 0;
    tile->setOnClick([&taps] { ++taps; });
    app.setRootWidget(std::move(tile));
    ASSERT_TRUE(app.renderFrame());

    EXPECT_TRUE(app.sendPointerDown(
        20.0f, 20.0f, 0, PointerSource::Touch, 81));
    EXPECT_TRUE(app.sendPointerUp(
        20.0f, 20.0f, 0, PointerSource::Touch, 81));
    EXPECT_EQ(taps, 1);
}

TEST(LclUiTest, MorphUsesFinalLayoutAndFreezesWindowInputUntilSettled) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 320, 200, "Morph transaction");
    auto root = std::make_unique<Container>();
    root->setWidth(320.0f);
    root->setHeight(200.0f);
    auto child = std::make_unique<Button>("Morph");
    Button* pointer = child.get();
    child->setWidth(100.0f);
    child->setHeight(40.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.animate(Motion::spring(0.32f, 0.06f), {LayoutMode::Morph}, [&] {
        pointer->setWidth(240.0f);
    });
    EXPECT_NEAR(pointer->getAbsoluteBounds().width, 240.0f, 0.01f);
    EXPECT_FALSE(app.sendPointerDown(20.0f, 20.0f));
    for (int index = 0; index < 240 && app.hasActiveAnimations(); ++index)
        app.advanceAnimations(1.0f / 240.0f);
    EXPECT_FALSE(app.hasActiveAnimations());
    EXPECT_TRUE(app.sendPointerDown(20.0f, 20.0f));
}

TEST(LclUiTest, MorphDoesNotBlendAClientRasterSnapshot) {
    WindowApp app(lcl::render::makeRasterCanvas(), 4, 4, "Retained morph");
    auto root = std::make_unique<Container>();
    Container* pointer = root.get();
    root->setWidth(4.0f);
    root->setHeight(4.0f);
    root->setBackgroundColor({255, 0, 0, 255});
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(app.getPixelBuffer()[0], 0xFFFF0000u);

    app.animate(Motion::tween(1.0f, Easing::linear()), {LayoutMode::Morph}, [&] {
        pointer->setBackgroundColor({0, 0, 255, 255});
    });

    // Morph animates retained presentation properties. It does not preserve,
    // blend, stretch or crossfade an old client-side pixel snapshot.
    EXPECT_EQ(pointer->getPresentationBackgroundColor().b, 255);
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(app.getPixelBuffer()[0], 0xFF0000FFu);
}

TEST(LclUiTest, MorphFinalUiStopsAnExistingPropertyTrack) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 40, 40, "Morph property interruption");
    auto root = std::make_unique<Container>();
    Container* pointer = root.get();
    root->setWidth(40.0f);
    root->setHeight(40.0f);
    root->setBackgroundColor({255, 0, 0, 255});
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.animate(Motion::tween(1.0f, Easing::linear()), [&] {
        pointer->setBackgroundColor({0, 255, 0, 255});
    });
    ASSERT_TRUE(app.advanceAnimations(0.25f));

    app.animate(Motion::tween(1.0f, Easing::linear()), {LayoutMode::Morph}, [&] {
        pointer->setBackgroundColor({0, 0, 255, 255});
    });
    EXPECT_EQ(pointer->getPresentationBackgroundColor().b, 255);

    EXPECT_FALSE(app.advanceAnimations(0.25f));
    EXPECT_EQ(pointer->getPresentationBackgroundColor().r, 0);
    EXPECT_EQ(pointer->getPresentationBackgroundColor().g, 0);
    EXPECT_EQ(pointer->getPresentationBackgroundColor().b, 255);
}

TEST(LclUiTest, ExplicitKeyframesArePresentationOnlyUntilCommittedAndSurviveCleanup) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 160, 100, "Keyframes");
    auto root = std::make_unique<Container>();
    root->setWidth(160.0f);
    root->setHeight(100.0f);
    Container* pointer = root.get();
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    AnimationOptions options;
    options.durationSec = 1.0f;
    options.fill = FillMode::Forwards;
    auto handle = pointer->animate(AnimatableProperty::Opacity,
        {{0.0f, 1.0f}, {1.0f, 0.0f}}, options);
    app.advanceAnimations(0.5f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.5f, 0.01f);
    EXPECT_FLOAT_EQ(pointer->getOpacity(), 1.0f);
    handle.finish();
    handle.commitFinalStyles();
    EXPECT_FLOAT_EQ(pointer->getOpacity(), 0.0f);

    auto replacement = std::make_unique<Container>();
    replacement->setWidth(160.0f);
    replacement->setHeight(100.0f);
    app.setRootWidget(std::move(replacement));
    EXPECT_NO_THROW(app.advanceAnimations(0.1f));
}

TEST(LclUiTest, IconUsesThePackagedSymbolFontFamily) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recording = canvas.get();
    WindowApp app(std::move(canvas), 80, 80, "Icon");
    auto icon = std::make_unique<Icon>(icons::Shield);
    icon->setWidth(32.0f);
    icon->setHeight(32.0f);
    icon->setIconSize(22.0f);
    app.setRootWidget(std::move(icon));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_FALSE(recording->fontFamilies.empty());
    EXPECT_EQ(recording->fontFamilies.back(), graphics::FontFamily::Icons);
}

TEST(LclUiTest, NavigationStackKeepsChromeAndOutgoingPageThroughPopAnimation) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 360, 640, "Navigation");
    const auto settleNavigation = [&](NavigationStack& navigation) {
        for (int frame = 0; frame < 180 && navigation.isTransitioning(); ++frame) {
            app.advanceAnimations(1.0f / 120.0f);
        }
    };
    auto navigation = std::make_unique<NavigationStack>();
    NavigationStack* stack = navigation.get();
    navigation->setWidth(360.0f);
    navigation->setHeight(640.0f);

    bool detailDestroyed = false;
    auto rootPage = std::make_unique<Container>();
    Container* rootPagePointer = rootPage.get();
    ASSERT_TRUE(navigation->setRootPage({
        .route = NavigationRoute("root"),
        .title = "Settings",
        .content = std::move(rootPage),
    }));
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());
    NavigationBar* persistentBar = &stack->navigationBar();

    auto detailPage = std::make_unique<Container>();
    detailPage->setDestructionCallback([&] { detailDestroyed = true; });
    ASSERT_TRUE(stack->push({
        .route = NavigationRoute("security"),
        .title = "Security",
        .content = std::move(detailPage),
    }));
    EXPECT_EQ(&stack->navigationBar(), persistentBar);
    EXPECT_EQ(stack->navigationBar().title(), "Security");
    EXPECT_EQ(stack->navigationBar().backTitle(), "Settings");
    EXPECT_TRUE(stack->navigationBar().canGoBack());
    EXPECT_TRUE(stack->isTransitioning());

    settleNavigation(*stack);
    EXPECT_FALSE(stack->isTransitioning());
    EXPECT_FALSE(detailDestroyed);
    EXPECT_TRUE(rootPagePointer->isVisible());
    EXPECT_FALSE(rootPagePointer->isInteractionEnabled());

    ASSERT_TRUE(stack->pop());
    EXPECT_EQ(stack->pageCount(), 1u);
    EXPECT_EQ(stack->navigationBar().title(), "Settings");
    EXPECT_FALSE(stack->navigationBar().canGoBack());
    EXPECT_FALSE(detailDestroyed);
    EXPECT_TRUE(rootPagePointer->isVisible());
    EXPECT_TRUE(rootPagePointer->isInteractionEnabled());
    settleNavigation(*stack);
    EXPECT_TRUE(detailDestroyed);
    EXPECT_FALSE(stack->isTransitioning());
    EXPECT_EQ(&stack->navigationBar(), persistentBar);
}

TEST(LclUiTest, NavigationStackRapidRouteChangeFinishesPreviousTransitionSafely) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 360, 640, "Navigation interrupt");
    auto navigation = std::make_unique<NavigationStack>();
    NavigationStack* stack = navigation.get();
    navigation->setWidth(360.0f);
    navigation->setHeight(640.0f);
    ASSERT_TRUE(navigation->setRootPage({
        .route = NavigationRoute("root"),
        .title = "Settings",
        .content = std::make_unique<Container>(),
    }));
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());

    ASSERT_TRUE(stack->push({
        .route = NavigationRoute("security"),
        .title = "Security",
        .content = std::make_unique<Container>(),
    }));
    ASSERT_TRUE(stack->pop());
    for (int frame = 0; frame < 180 && stack->isTransitioning(); ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
    }
    ASSERT_NE(stack->currentRoute(), nullptr);
    EXPECT_EQ(stack->currentRoute()->value, "root");
    EXPECT_EQ(stack->pageCount(), 1u);
    EXPECT_FALSE(stack->isTransitioning());
}

TEST(LclUiTest, NavigationStackEdgeSwipeTracksFingerAndCommitsPop) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 360, 640, "Navigation edge swipe");
    auto navigation = std::make_unique<NavigationStack>();
    NavigationStack* stack = navigation.get();
    navigation->setWidth(360.0f);
    navigation->setHeight(640.0f);
    auto rootPage = std::make_unique<Container>();
    Container* rootPointer = rootPage.get();
    ASSERT_TRUE(navigation->setRootPage({
        .route = NavigationRoute("root"),
        .title = "Settings",
        .content = std::move(rootPage),
    }));
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());

    auto detailPage = std::make_unique<Container>();
    Container* detailPointer = detailPage.get();
    ASSERT_TRUE(stack->push({
        .route = NavigationRoute("security"),
        .title = "Security",
        .content = std::move(detailPage),
    }));
    for (int frame = 0; frame < 180 && stack->isTransitioning(); ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
    }
    ASSERT_FALSE(stack->isTransitioning());

    app.sendPointerDown(4.0f, 100.0f, 0, PointerSource::Mouse, 41);
    app.sendPointerMove(150.0f, 103.0f, PointerSource::Mouse, 41);
    EXPECT_FLOAT_EQ(detailPointer->getPresentationState().translationX, 0.0f);
    app.sendPointerUp(150.0f, 103.0f, 0, PointerSource::Mouse, 41);
    EXPECT_EQ(stack->pageCount(), 2u);
    EXPECT_FALSE(stack->isTransitioning());

    app.sendPointerDown(4.0f, 100.0f, 0, PointerSource::Touch, 91);
    ASSERT_TRUE(app.sendPointerMove(
        150.0f, 103.0f, PointerSource::Touch, 91));
    EXPECT_GT(detailPointer->getPresentationState().translationX, 100.0f);
    EXPECT_LT(rootPointer->getPresentationState().translationX, 0.0f);
    EXPECT_EQ(stack->pageCount(), 2u);
    app.sendPointerUp(150.0f, 103.0f, 0, PointerSource::Touch, 91);
    EXPECT_EQ(stack->pageCount(), 1u);
    ASSERT_NE(stack->currentRoute(), nullptr);
    EXPECT_EQ(stack->currentRoute()->value, "root");
    for (int frame = 0; frame < 180 && stack->isTransitioning(); ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
    }
    EXPECT_FALSE(stack->isTransitioning());
    EXPECT_TRUE(rootPointer->isInteractionEnabled());
}

TEST(LclUiTest, NavigationStackEdgeSwipeBelowThresholdSpringsBack) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 360, 640, "Navigation edge cancel");
    auto navigation = std::make_unique<NavigationStack>();
    NavigationStack* stack = navigation.get();
    navigation->setWidth(360.0f);
    navigation->setHeight(640.0f);
    ASSERT_TRUE(navigation->setRootPage({
        .route = NavigationRoute("root"),
        .title = "Settings",
        .content = std::make_unique<Container>(),
    }));
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());
    auto detailPage = std::make_unique<Container>();
    Container* detailPointer = detailPage.get();
    ASSERT_TRUE(stack->push({
        .route = NavigationRoute("security"),
        .title = "Security",
        .content = std::move(detailPage),
    }));
    for (int frame = 0; frame < 180 && stack->isTransitioning(); ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
    }

    app.sendPointerDown(4.0f, 100.0f, 0, PointerSource::Touch, 92);
    ASSERT_TRUE(app.sendPointerMove(
        70.0f, 101.0f, PointerSource::Touch, 92));
    app.sendPointerUp(70.0f, 101.0f, 0, PointerSource::Touch, 92);
    EXPECT_EQ(stack->pageCount(), 2u);
    for (int frame = 0; frame < 180 && stack->isTransitioning(); ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
    }
    EXPECT_FALSE(stack->isTransitioning());
    ASSERT_NE(stack->currentRoute(), nullptr);
    EXPECT_EQ(stack->currentRoute()->value, "security");
    EXPECT_NEAR(detailPointer->getPresentationState().translationX, 0.0f, 0.01f);
    EXPECT_TRUE(detailPointer->isInteractionEnabled());
}

TEST(LclUiTest, NavigationSplitViewPreservesItsPanesAcrossSizeClassChanges) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 900, 640, "Adaptive navigation");
    auto navigation = std::make_unique<NavigationSplitView>();
    NavigationSplitView* split = navigation.get();
    navigation->setWidth(900.0f);
    navigation->setHeight(640.0f);

    bool sidebarDestroyed = false;
    auto sidebar = std::make_unique<Container>();
    Container* sidebarPointer = sidebar.get();
    sidebar->setDestructionCallback([&] { sidebarDestroyed = true; });
    split->setSidebar(std::move(sidebar));
    ASSERT_TRUE(split->detailNavigation().setRootPage({
        .route = NavigationRoute("general"),
        .title = "General",
        .content = std::make_unique<Container>(),
    }));
    NavigationStack* detailPointer = &split->detailNavigation();
    split->setSizeClass(LayoutSizeClass::Expanded);
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());

    EXPECT_FALSE(sidebarDestroyed);
    EXPECT_EQ(&split->detailNavigation(), detailPointer);
    EXPECT_EQ(sidebarPointer->getParent()->getParent()->getParent(), split);
    EXPECT_NEAR(detailPointer->getBounds().x, 292.0f, 0.01f);
    EXPECT_EQ(detailPointer->pageCount(), 1u);

    split->setSizeClass(LayoutSizeClass::Compact);
    ASSERT_TRUE(app.renderFrame());
    EXPECT_FALSE(sidebarDestroyed);
    EXPECT_EQ(&split->detailNavigation(), detailPointer);
    EXPECT_EQ(sidebarPointer->getParent()->getParent()->getParent(), split);
    EXPECT_NEAR(detailPointer->getBounds().x, 0.0f, 0.01f);
    EXPECT_EQ(detailPointer->pageCount(), 1u);
}

TEST(LclUiTest, NavigationSplitViewOnlyTouchEdgeSwipeReturnsToPrimary) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 360, 640, "Adaptive navigation edge swipe");
    auto navigation = std::make_unique<NavigationSplitView>();
    NavigationSplitView* split = navigation.get();
    navigation->setWidth(360.0f);
    navigation->setHeight(640.0f);
    split->setSidebar(std::make_unique<Container>());
    ASSERT_TRUE(split->setDetailPage({
        .route = NavigationRoute("general"),
        .title = "General",
        .content = std::make_unique<Container>(),
    }, PageTransition::None));
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());
    ASSERT_TRUE(split->isDetailPresented());

    app.sendPointerDown(4.0f, 100.0f, 0, PointerSource::Mouse, 41);
    app.sendPointerMove(150.0f, 102.0f, PointerSource::Mouse, 41);
    EXPECT_FLOAT_EQ(
        split->detailNavigation().getPresentationState().translationX, 0.0f);
    app.sendPointerUp(150.0f, 102.0f, 0, PointerSource::Mouse, 41);
    EXPECT_TRUE(split->isDetailPresented());
    EXPECT_FALSE(split->isTransitioning());

    app.sendPointerDown(4.0f, 100.0f, 0, PointerSource::Touch, 42);
    ASSERT_TRUE(app.sendPointerMove(
        150.0f, 102.0f, PointerSource::Touch, 42));
    EXPECT_GT(split->detailNavigation().getPresentationState().translationX,
              100.0f);
    app.sendPointerUp(150.0f, 102.0f, 0, PointerSource::Touch, 42);
    EXPECT_FALSE(split->isDetailPresented());
    for (int frame = 0; frame < 180 && split->isTransitioning(); ++frame) {
        app.advanceAnimations(1.0f / 120.0f);
    }
    EXPECT_FALSE(split->isTransitioning());
    EXPECT_FALSE(split->isDetailPresented());
}

TEST(LclUiTest, NavigationSplitViewAnimatesCompactDetailReplacement) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 360, 640, "Compact detail replacement");
    auto navigation = std::make_unique<NavigationSplitView>();
    NavigationSplitView* split = navigation.get();
    navigation->setWidth(360.0f);
    navigation->setHeight(640.0f);
    split->setSidebar(std::make_unique<Container>());
    ASSERT_TRUE(split->detailNavigation().setRootPage({
        .route = NavigationRoute("general"),
        .title = "General",
        .content = std::make_unique<Container>(),
    }));
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());

    ASSERT_TRUE(split->setDetailPage({
        .route = NavigationRoute("privacy"),
        .title = "Privacy",
        .content = std::make_unique<Container>(),
    }, PageTransition::Replace));
    EXPECT_TRUE(split->isTransitioning());
}

TEST(LclUiTest, NavigationSplitViewPreparesAlreadyCurrentCompactDetail) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 360, 640, "Compact current detail");
    auto navigation = std::make_unique<NavigationSplitView>();
    NavigationSplitView* split = navigation.get();
    navigation->setWidth(360.0f);
    navigation->setHeight(640.0f);
    split->setSidebar(std::make_unique<Container>());
    ASSERT_TRUE(split->detailNavigation().setRootPage({
        .route = NavigationRoute("general"),
        .title = "General",
        .content = std::make_unique<Container>(),
    }));
    app.setRootWidget(std::move(navigation));
    ASSERT_TRUE(app.renderFrame());

    split->presentDetail();

    EXPECT_TRUE(split->isTransitioning());
    EXPECT_TRUE(split->isDetailPresented());
    EXPECT_FLOAT_EQ(split->detailNavigation().getPresentationState().opacity,
                    0.0f);
}

TEST(LclUiTest, NavigationSplitViewCompletesWhenRetainedAnimationIsUnavailable) {
    MotionCoordinator coordinator;
    coordinator.setCompositorAnimationDelegate(
        [](Widget&, AnimatableProperty, float, float, float,
           const lcl::motion::Motion&) {
            // Connected WindowApp uses this result when no retained cache is
            // eligible: the model is committed directly and no compositor
            // completion callback will follow.
            return true;
        });

    NavigationSplitView split;
    split.setWidth(360.0f);
    split.setHeight(640.0f);
    split.setSidebar(std::make_unique<Container>());
    ASSERT_TRUE(split.detailNavigation().setRootPage({
        .route = NavigationRoute("general"),
        .title = "General",
        .content = std::make_unique<Container>(),
    }));
    split.setMotionCoordinator(&coordinator);
    split.calculateLayout(360.0f, 640.0f);
    split.syncLayout();

    split.presentDetail();
    ASSERT_TRUE(split.isTransitioning());
    coordinator.tick(1.0f / 60.0f);
    coordinator.tick(1.0f / 60.0f);
    coordinator.tick(1.0f / 60.0f);

    EXPECT_FALSE(split.isTransitioning());
    EXPECT_TRUE(split.isDetailPresented());
    EXPECT_TRUE(split.detailNavigation().isInteractionEnabled());
    EXPECT_FLOAT_EQ(
        split.detailNavigation().getPresentationState().opacity, 1.0f);
    EXPECT_NEAR(
        split.detailNavigation().getPresentationState().translationX,
        0.0f, 0.01f);
    ASSERT_FALSE(split.getChildren().empty());
    EXPECT_NEAR(split.getChildren().front()->getPresentationState().translationX,
                -180.0f, 0.01f);
}

TEST(LclUiTest, ToggleDefaultsProgrammaticValueAndNotificationsAreDeterministic) {
    Toggle toggle;
    EXPECT_FALSE(toggle.value());
    EXPECT_TRUE(toggle.isEnabled());
    EXPECT_TRUE(toggle.isFocusable());

    std::vector<bool> changes;
    toggle.setOnChange([&](bool value) { changes.push_back(value); });
    toggle.setValue(false);
    EXPECT_TRUE(changes.empty());

    toggle.setValue(true);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(changes.back());
    EXPECT_TRUE(toggle.value());

    toggle.setValue(true);
    EXPECT_EQ(changes.size(), 1u);

    toggle.setValue(false);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_FALSE(changes.back());

    Toggle initiallyOn(true);
    EXPECT_TRUE(initiallyOn.value());
}

TEST(LclUiTest, ToggleMouseAndTouchUseOneValueTransitionPath) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 100, 70, "Toggle pointer input");
    auto toggle = std::make_unique<Toggle>();
    Toggle* pointer = toggle.get();
    int changes = 0;
    toggle->setOnChange([&](bool) { ++changes; });
    app.setRootWidget(std::move(toggle));
    app.updateLayout();

    EXPECT_FALSE(app.sendPointerDown(20.0f, 22.0f, 1));
    EXPECT_FALSE(app.sendPointerUp(20.0f, 22.0f, 1));
    EXPECT_FALSE(pointer->value());
    EXPECT_EQ(changes, 0);

    EXPECT_TRUE(app.sendPointerDown(20.0f, 22.0f));
    EXPECT_FALSE(pointer->value());
    EXPECT_TRUE(app.sendPointerUp(20.0f, 22.0f));
    EXPECT_TRUE(pointer->value());
    EXPECT_EQ(changes, 1);

    EXPECT_TRUE(app.sendPointerDown(
        20.0f, 22.0f, 0, PointerSource::Touch, 7));
    EXPECT_TRUE(pointer->value());
    EXPECT_TRUE(app.sendPointerUp(
        20.0f, 22.0f, 0, PointerSource::Touch, 7));
    EXPECT_FALSE(pointer->value());
    EXPECT_EQ(changes, 2);

    EXPECT_TRUE(app.sendPointerDown(
        20.0f, 22.0f, 0, PointerSource::Touch, 8));
    EXPECT_FALSE(app.sendPointerMove(
        35.0f, 22.0f, PointerSource::Touch, 8));
    EXPECT_TRUE(app.sendPointerUp(
        35.0f, 22.0f, 0, PointerSource::Touch, 8));
    EXPECT_FALSE(pointer->value());
    EXPECT_EQ(changes, 2);

    EXPECT_TRUE(app.sendPointerDown(
        20.0f, 22.0f, 0, PointerSource::Touch, 9));
    EXPECT_TRUE(app.sendPointerCancel(
        20.0f, 22.0f, PointerSource::Touch, 9));
    EXPECT_FALSE(pointer->value());
    EXPECT_EQ(changes, 2);

    pointer->setEnabled(false);
    EXPECT_FALSE(app.sendPointerDown(20.0f, 22.0f));
    EXPECT_FALSE(app.sendPointerUp(20.0f, 22.0f));
    EXPECT_FALSE(app.sendPointerDown(
        20.0f, 22.0f, 0, PointerSource::Touch, 10));
    EXPECT_FALSE(app.sendPointerUp(
        20.0f, 22.0f, 0, PointerSource::Touch, 10));
    EXPECT_FALSE(pointer->value());
    EXPECT_EQ(changes, 2);
}

TEST(LclUiTest, ToggleFocusedSpaceAndWindowTraversalRespectEligibility) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 180, 160, "Toggle keyboard input");
    auto root = std::make_unique<Container>();
    root->setWidth(180.0f);
    root->setHeight(160.0f);
    root->setDirection(layout::Direction::Column);

    auto before = std::make_unique<Button>("Before");
    Button* beforePtr = before.get();
    before->setHeight(40.0f);
    auto toggle = std::make_unique<Toggle>();
    Toggle* togglePtr = toggle.get();
    auto after = std::make_unique<TextField>();
    TextField* afterPtr = after.get();
    after->setHeight(40.0f);

    root->addChild(std::move(before));
    root->addChild(std::move(toggle));
    root->addChild(std::move(after));
    app.setRootWidget(std::move(root));
    app.updateLayout();

    const int tab = static_cast<int>(lcl::platform::PhysicalKey::Tab);
    const int space = static_cast<int>(lcl::platform::PhysicalKey::Space);
    const int enter = static_cast<int>(lcl::platform::PhysicalKey::Enter);

    EXPECT_FALSE(app.sendKeyDown(space));
    EXPECT_FALSE(togglePtr->value());
    app.getDispatcher().setFocus(beforePtr);
    EXPECT_TRUE(app.sendKeyDown(tab));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), togglePtr);
    EXPECT_TRUE(app.sendKeyDown(space));
    EXPECT_TRUE(togglePtr->value());
    EXPECT_FALSE(app.sendKeyDown(enter));
    EXPECT_TRUE(togglePtr->value());
    EXPECT_TRUE(app.sendKeyDown(tab));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), afterPtr);

    togglePtr->setEnabled(false);
    app.getDispatcher().setFocus(beforePtr);
    EXPECT_TRUE(app.sendKeyDown(tab));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), afterPtr);
    app.getDispatcher().setFocus(togglePtr);
    EXPECT_NE(app.getDispatcher().getFocusedWidget(), togglePtr);
    EXPECT_FALSE(togglePtr->onKeyDown(KeyEvent{
        lcl::platform::PhysicalKey::Space, space}));
    EXPECT_TRUE(togglePtr->value());
}

TEST(LclUiTest, ToggleValueIsImmediateWhileThumbAnimationRetargetsOneChannel) {
    MotionCoordinator coordinator;
    Toggle toggle;
    toggle.setMotionCoordinator(&coordinator);
    toggle.calculateLayout(60.0f, 44.0f);
    toggle.syncLayout();
    const uint64_t objectId = toggle.getObjectId();

    toggle.setValue(true);
    EXPECT_TRUE(toggle.value());
    EXPECT_TRUE(coordinator.isObjectAnimating(objectId));

    RecordingCanvas canvas;
    const graphics::RectF damage{-10.0f, -10.0f, 100.0f, 80.0f};
    toggle.draw(canvas, damage);
    ASSERT_EQ(canvas.roundedRects.size(), 3u);
    const float offX = canvas.roundedRects.back().x;

    coordinator.tick(0.08f);
    canvas.roundedRects.clear();
    toggle.draw(canvas, damage);
    ASSERT_EQ(canvas.roundedRects.size(), 3u);
    EXPECT_GT(canvas.roundedRects.back().x, offX);

    toggle.setValue(false);
    EXPECT_FALSE(toggle.value());
    EXPECT_TRUE(coordinator.isObjectAnimating(objectId));
    coordinator.tick(1.0f);

    canvas.roundedRects.clear();
    toggle.draw(canvas, damage);
    ASSERT_EQ(canvas.roundedRects.size(), 3u);
    EXPECT_NEAR(canvas.roundedRects.back().x, offX, 0.01f);
}

TEST(LclUiTest, ToggleThumbTargetsAndLogicalGeometryStayStableAcrossCanvasScale) {
    Toggle toggle;
    toggle.calculateLayout(60.0f, 44.0f);
    toggle.syncLayout();
    const graphics::RectF damage{-10.0f, -10.0f, 100.0f, 80.0f};

    RecordingCanvas canvas;
    canvas.setRenderTarget({{60.0f, 44.0f}, {60, 44}, 1.0f});
    toggle.draw(canvas, damage);
    ASSERT_EQ(canvas.roundedRects.size(), 3u);
    const graphics::RectF offTrack = canvas.roundedRects.front();
    const graphics::RectF offThumb = canvas.roundedRects.back();
    EXPECT_FLOAT_EQ(offTrack.width, 52.0f);
    EXPECT_FLOAT_EQ(offTrack.height, 32.0f);
    EXPECT_FLOAT_EQ(offThumb.x, 7.0f);
    EXPECT_FLOAT_EQ(offThumb.width, 26.0f);

    toggle.setValue(true);
    canvas.roundedRects.clear();
    canvas.setRenderTarget({{60.0f, 44.0f}, {120, 88}, 2.0f});
    toggle.draw(canvas, damage);
    ASSERT_EQ(canvas.roundedRects.size(), 3u);
    const graphics::RectF onTrack = canvas.roundedRects.front();
    const graphics::RectF onThumb = canvas.roundedRects.back();
    EXPECT_FLOAT_EQ(onTrack.x, offTrack.x);
    EXPECT_FLOAT_EQ(onTrack.y, offTrack.y);
    EXPECT_FLOAT_EQ(onTrack.width, offTrack.width);
    EXPECT_FLOAT_EQ(onTrack.height, offTrack.height);
    EXPECT_FLOAT_EQ(onThumb.x, 27.0f);
    EXPECT_FLOAT_EQ(onThumb.width, offThumb.width);
}

TEST(LclUiTest, ToggleInteractionPresentationAndActiveAnimationCleanUpSafely) {
    MotionCoordinator coordinator;
    auto toggle = std::make_unique<Toggle>();
    toggle->setMotionCoordinator(&coordinator);
    const uint64_t objectId = toggle->getObjectId();

    toggle->onPointerEnter(PointerEvent{
        10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Enter});
    coordinator.tick(1.0f);
    EXPECT_NEAR(toggle->getPresentationState().scaleX, 1.015f, 0.001f);
    toggle->onPointerDown(PointerEvent{
        10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down});
    coordinator.tick(1.0f);
    EXPECT_NEAR(toggle->getPresentationState().scaleX, 0.965f, 0.001f);

    toggle->setValue(true);
    EXPECT_TRUE(coordinator.isObjectAnimating(objectId));
    toggle.reset();
    EXPECT_FALSE(coordinator.isObjectAnimating(objectId));
    EXPECT_NO_THROW(coordinator.tick(1.0f));
}

TEST(LclUiTest, ButtonInteractionMotionComposesHoverPressFocusDisabledAndStyleOverride) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 240, 120, "Interaction motion");
    auto styleTheme = lcl::theme::createDefaultTheme();
    styleTheme.primaryButton.hover.scale = 1.10f;
    app.setTheme(std::move(styleTheme));
    auto root = std::make_unique<Container>();
    root->setWidth(240.0f); root->setHeight(120.0f);
    auto button = std::make_unique<Button>("Motion");
    Button* pointer = button.get();
    button->setWidth(100.0f); button->setHeight(40.0f);
    root->addChild(std::move(button));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    EXPECT_FALSE(app.sendPointerMove(20.0f, 20.0f));
    for (int index = 0; index < 100; ++index) app.advanceAnimations(1.0f / 240.0f);
    EXPECT_EQ(pointer->getState(), ButtonState::Hover);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 1.10f, 0.01f);

    EXPECT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(pointer->getState(), ButtonState::Active);
    EXPECT_TRUE(app.sendPointerUp(20.0f, 20.0f));
    EXPECT_EQ(pointer->getState(), ButtonState::Hover);
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), pointer);

    pointer->setEnabled(false);
    EXPECT_EQ(pointer->getState(), ButtonState::Disabled);
    pointer->setEnabled(true);
    EXPECT_EQ(pointer->getState(), ButtonState::Focused);

    InteractionMotionTheme motionTheme;
    motionTheme.enabled = false;
    pointer->setInteractionMotionTheme(motionTheme);
    pointer->setEnabled(false);
    pointer->setEnabled(true);
    EXPECT_FLOAT_EQ(pointer->getPresentationState().scaleX, 1.0f);
}

TEST(LclUiTest, CustomContainerUsesDeclarativeHoverPressedAndClickStates) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 200, 100, "Declarative interaction");
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(100.0f);
    auto control = std::make_unique<Container>();
    Container* pointer = control.get();
    control->setWidth(100.0f);
    control->setHeight(40.0f);
    control->setInteractionStyle(InteractionState::Normal,
        InteractionStyle{.scale = 1.0f, .opacity = 1.0f,
                         .motion = Motion::tween(0.01f, Easing::linear())});
    control->setInteractionStyle(InteractionState::Hover,
        InteractionStyle{.scale = 1.10f, .opacity = 0.90f,
                         .motion = Motion::tween(0.01f, Easing::linear())});
    control->setInteractionStyle(InteractionState::Pressed,
        InteractionStyle{.scale = 0.92f, .opacity = 0.80f,
                         .motion = Motion::tween(0.01f, Easing::linear())});
    int clicks = 0;
    control->setOnClick([&] { ++clicks; });
    root->addChild(std::move(control));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    app.sendPointerMove(20.0f, 20.0f);
    app.advanceAnimations(0.02f);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 1.10f, 0.001f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.90f, 0.001f);

    EXPECT_TRUE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(app.getDispatcher().getFocusedWidget(), pointer);
    app.advanceAnimations(0.02f);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 0.92f, 0.001f);
    EXPECT_NEAR(pointer->getPresentationState().opacity, 0.80f, 0.001f);

    EXPECT_TRUE(app.sendPointerUp(20.0f, 20.0f));
    EXPECT_EQ(clicks, 1);
    app.advanceAnimations(0.02f);
    EXPECT_NEAR(pointer->getPresentationState().scaleX, 1.10f, 0.001f);

    pointer->setInteractionEnabled(false);
    EXPECT_FALSE(app.sendPointerDown(20.0f, 20.0f));
    EXPECT_EQ(clicks, 1);
}

TEST(LclUiTest, TextUsesCanonicalSkiaCommandDuringAncestorAnimation) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 240, 120, "Animated text raster layer");
    auto root = std::make_unique<Container>();
    root->setWidth(240.0f);
    root->setHeight(120.0f);
    auto button = std::make_unique<Button>("Stable");
    button->setWidth(100.0f);
    button->setHeight(40.0f);
    root->addChild(std::move(button));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(recorded->texts.size(), 1u);

    app.sendPointerMove(20.0f, 20.0f);
    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(recorded->texts.size(), 2u);
    EXPECT_EQ(recorded->texts.back(), "Stable");

    for (int index = 0; index < 300 && app.hasActiveAnimations(); ++index) {
        app.advanceAnimations(1.0f / 240.0f);
    }
    ASSERT_FALSE(app.hasActiveAnimations());
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(recorded->texts.size(), 3u);
}

TEST(LclUiTest, WindowAppRendersReplacementRootAfterInitialFrame) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 64, 48, "Replacement root test");

    auto initialRoot = std::make_unique<Container>();
    initialRoot->setBackgroundColor({1, 2, 3, 255});
    initialRoot->setWidth(64.0f);
    initialRoot->setHeight(48.0f);
    app.setRootWidget(std::move(initialRoot));
    ASSERT_TRUE(app.renderFrame());
    ASSERT_FALSE(app.renderFrame());

    recorded->rects.clear();
    recorded->colors.clear();

    auto replacementRoot = std::make_unique<Container>();
    replacementRoot->setBackgroundColor({20, 40, 60, 255});
    replacementRoot->setWidth(64.0f);
    replacementRoot->setHeight(48.0f);
    app.setRootWidget(std::move(replacementRoot));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(recorded->rects.size(), 1u);
    EXPECT_EQ(recorded->rects.front().width, 64.0f);
    EXPECT_EQ(recorded->rects.front().height, 48.0f);
    ASSERT_EQ(recorded->colors.size(), 1u);
    EXPECT_EQ(recorded->colors.front().r, 20);
    EXPECT_EQ(recorded->colors.front().g, 40);
    EXPECT_EQ(recorded->colors.front().b, 60);
}

TEST(LclUiTest, WindowAppInvokesResizeLifecycleAfterLogicalResize) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Resize callback test");

    uint32_t callbackWidth = 0;
    uint32_t callbackHeight = 0;
    app.setOnResize([&](uint32_t width, uint32_t height) {
        callbackWidth = width;
        callbackHeight = height;
    });

    app.resize(120, 72);

    EXPECT_EQ(app.getWidth(), 120u);
    EXPECT_EQ(app.getHeight(), 72u);
    EXPECT_EQ(callbackWidth, 120u);
    EXPECT_EQ(callbackHeight, 72u);
}

TEST(LclUiTest, AbsoluteEdgePinnedLayerTracksWindowResize) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Edge-pinned resize test");

    auto root = std::make_unique<Container>();
    root->setWidth(64.0f);
    root->setHeight(48.0f);
    auto layer = std::make_unique<Container>();
    Container* layerPointer = layer.get();
    layer->setPositionType(layout::PositionType::Absolute);
    layer->setPosition(layout::Edge::Left, 0.0f);
    layer->setPosition(layout::Edge::Top, 0.0f);
    layer->setPosition(layout::Edge::Right, 0.0f);
    layer->setPosition(layout::Edge::Bottom, 0.0f);
    root->addChild(std::move(layer));
    app.setRootWidget(std::move(root));

    app.updateLayout();
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().width, 64.0f);
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().height, 48.0f);

    app.resize(120, 72);
    app.updateLayout();
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().width, 120.0f);
    EXPECT_FLOAT_EQ(layerPointer->getAbsoluteBounds().height, 72.0f);
}

TEST(LclUiTest, WindowAppStagesSurfaceChromeBeforeCompositorConnection) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Staged chrome test");

    EXPECT_TRUE(app.setDecorationMode(lcl::protocol::LCLDecorationMode::CSD));
    EXPECT_TRUE(app.setEdgeToEdge(true));
    EXPECT_TRUE(app.setWindowCornerRadius(14.0f));
}

TEST(LclUiTest, WindowAppSendsExplicitEdgeToEdgeState) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Edge-to-edge test");
    app.setSurfaceId(5);
    app.setExternalIpcSocket(sockets[0]);

    ASSERT_TRUE(app.setEdgeToEdge(true));

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::SetEdgeToEdge);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgSetEdgeToEdge));
    const auto* edgeToEdge =
        reinterpret_cast<const lcl::protocol::LCLMsgSetEdgeToEdge*>(
            payload.data());
    EXPECT_EQ(edgeToEdge->surfaceId, 5u);
    EXPECT_EQ(edgeToEdge->enabled, 1u);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, WindowAppSendsOneWindowCornerStyle) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Corner style test");
    app.setSurfaceId(5);
    app.setExternalIpcSocket(sockets[0]);

    ASSERT_TRUE(app.setWindowCornerStyle(20.0f, 3.2f));

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::SetWindowCornerStyle);
    ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgSetWindowCornerStyle));
    const auto* style = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowCornerStyle*>(payload.data());
    EXPECT_EQ(style->surfaceId, 5u);
    EXPECT_FLOAT_EQ(style->radius, 20.0f);
    EXPECT_FLOAT_EQ(style->roundness, 3.2f);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, WindowAppCsdControlsAndCustomRequestsUseWindowActions) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 540, 360, "Window action test");
    app.setSurfaceId(9);
    app.setExternalIpcSocket(sockets[0]);

    const lcl::ui::chrome::WindowChromeStyle style;
    auto titlebar = lcl::ui::chrome::buildWindowTitlebar(
        540.0f, 34.0f, 20.0f, "Window action test", 14.0f, style,
        lcl::ui::chrome::WindowChromeActions{
            .close = [&app] { return app.requestWindowClose(); },
            .minimize = [&app] { return app.requestWindowMinimize(); },
            .toggleMaximize = [&app] {
                return app.requestWindowToggleMaximize();
            },
            .beginDrag = [&app](float x, float y) {
                return app.requestWindowDrag(x, y);
            },
        });
    const auto layout = titlebar->chromeLayout();
    app.setRootWidget(std::move(titlebar));
    ASSERT_TRUE(app.renderFrame());

    const auto expectAction = [&](lcl::protocol::LCLWindowAction expected) {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        ASSERT_TRUE(lcl::protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
        EXPECT_EQ(header.opcode, lcl::protocol::LCLOpcode::RequestWindowAction);
        ASSERT_EQ(payload.size(), sizeof(lcl::protocol::LCLMsgRequestWindowAction));
        const auto* message = reinterpret_cast<const lcl::protocol::LCLMsgRequestWindowAction*>(payload.data());
        EXPECT_EQ(message->surfaceId, 9u);
        EXPECT_EQ(message->action, expected);
    };

    const auto expectNoAction = [&] {
        char byte = 0;
        errno = 0;
        EXPECT_EQ(recv(sockets[1], &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT), -1);
        EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    };

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft + 1.0f, layout.controlTop + 1.0f));
    expectNoAction();
    EXPECT_TRUE(app.sendPointerUp(layout.controlLeft + 1.0f, layout.controlTop + 1.0f));
    expectAction(lcl::protocol::LCLWindowAction::Close);

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft + style.controlSize + style.controlGap + 1.0f,
                                    layout.controlTop + 1.0f));
    expectNoAction();
    EXPECT_TRUE(app.sendPointerUp(layout.controlLeft + style.controlSize + style.controlGap + 1.0f,
                                  layout.controlTop + 1.0f));
    expectAction(lcl::protocol::LCLWindowAction::Minimize);

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft +
                                        2.0f * (style.controlSize + style.controlGap) + 1.0f,
                                    layout.controlTop + 1.0f));
    expectNoAction();
    EXPECT_TRUE(app.sendPointerUp(layout.controlLeft +
                                      2.0f * (style.controlSize + style.controlGap) + 1.0f,
                                  layout.controlTop + 1.0f));
    expectAction(lcl::protocol::LCLWindowAction::ToggleMaximize);

    EXPECT_TRUE(app.sendPointerDown(layout.controlLeft + 1.0f, layout.controlTop + 1.0f));
    EXPECT_TRUE(app.sendPointerUp(300.0f, 100.0f));
    expectNoAction();

    EXPECT_TRUE(app.sendPointerDown(300.0f, 8.0f));
    expectAction(lcl::protocol::LCLWindowAction::BeginDrag);

    EXPECT_TRUE(app.requestWindowRestore());
    expectAction(lcl::protocol::LCLWindowAction::Restore);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, PassiveBackdropSurfaceKeepsItsVisualStateOnPointerEvents) {
    BackdropSurface surface;
    surface.setBackgroundColor({17, 19, 23, 184});
    surface.setInteractive(false);

    PointerEvent event{};
    EXPECT_FALSE(surface.onPointerEnter(event));
    EXPECT_FALSE(surface.onPointerDown(event));
    EXPECT_EQ(surface.getBackgroundColor().r, 17);
    EXPECT_EQ(surface.getBackgroundColor().a, 184);
}

TEST(LclUiTest, GlassUsesTheGenericAddFilterChain) {
    BackdropSurface surface;
    surface.setWidth(100.0f);
    surface.setHeight(100.0f);
    surface.setBorderRoundness(3.2f);
    surface.setEffectBounds(EffectBounds::OuterSurface);
    surface.addFilter(lcl::protocol::FilterType::Blur, 8.0f);
    surface.addFilter(lcl::protocol::FilterType::Glass, 30.0f, 3.0f, 12.0f);
    surface.setTint({15, 23, 42, 128});
    surface.calculateLayout(100.0f, 100.0f);
    surface.syncLayout();

    std::vector<EffectRegion> effects;
    surface.collectEffects(effects);

    ASSERT_EQ(effects.size(), 1u);
    ASSERT_EQ(effects.front().filters.size(), 3u);
    EXPECT_FLOAT_EQ(effects.front().cornerRoundness, 3.2f);
    EXPECT_EQ(effects.front().boundsPolicy, EffectBounds::OuterSurface);
    EXPECT_EQ(effects.front().filters[0].type, lcl::protocol::FilterType::Blur);
    const auto& glass = effects.front().filters[1];
    EXPECT_EQ(glass.type, lcl::protocol::FilterType::Glass);
    EXPECT_FLOAT_EQ(glass.value, 1.0f);
    EXPECT_FLOAT_EQ(glass.params[0], 30.0f);
    EXPECT_FLOAT_EQ(glass.params[1], 3.0f);
    EXPECT_FLOAT_EQ(glass.params[2], 12.0f);
    const auto& tint = effects.front().filters[2];
    EXPECT_EQ(tint.type, lcl::protocol::FilterType::Tint);
    EXPECT_FLOAT_EQ(tint.value, 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(tint.params[0], 15.0f);
    EXPECT_FLOAT_EQ(tint.params[1], 23.0f);
    EXPECT_FLOAT_EQ(tint.params[2], 42.0f);
}

TEST(LclUiTest, GlassPreservesZeroControlsInsteadOfSynthesizingDefaults) {
    BackdropSurface surface;
    surface.setWidth(32.0f);
    surface.setHeight(24.0f);
    surface.addFilter(lcl::protocol::FilterType::Glass, 0.0f, 0.0f, 0.0f);
    surface.calculateLayout(32.0f, 24.0f);
    surface.syncLayout();

    std::vector<EffectRegion> effects;
    surface.collectEffects(effects);

    ASSERT_EQ(effects.size(), 1u);
    ASSERT_EQ(effects.front().filters.size(), 1u);
    const auto& glass = effects.front().filters.front();
    EXPECT_EQ(glass.type, lcl::protocol::FilterType::Glass);
    EXPECT_FLOAT_EQ(glass.params[0], 0.0f);
    EXPECT_FLOAT_EQ(glass.params[1], 0.0f);
    EXPECT_FLOAT_EQ(glass.params[2], 0.0f);
}

TEST(LclUiTest, GlassRefractionPassScalePreservesLogicalDisplacementAcrossDpr) {
    const auto scale1 = lcl::render::computeBackdropPassScale(1.0f, 100, 80, 100, 80);
    const auto scale125 = lcl::render::computeBackdropPassScale(1.25f, 125, 100, 125, 100);
    const auto scale15 = lcl::render::computeBackdropPassScale(1.5f, 150, 120, 150, 120);
    const auto scale2 = lcl::render::computeBackdropPassScale(2.0f, 200, 160, 200, 160);

    EXPECT_FLOAT_EQ(scale1.x, 1.0f);
    EXPECT_FLOAT_EQ(scale125.x, 1.25f);
    EXPECT_FLOAT_EQ(scale15.x, 1.5f);
    EXPECT_FLOAT_EQ(scale2.x, 2.0f);
    EXPECT_FLOAT_EQ(scale1.y, 1.0f);
    EXPECT_FLOAT_EQ(scale125.y, 1.25f);
    EXPECT_FLOAT_EQ(scale15.y, 1.5f);
    EXPECT_FLOAT_EQ(scale2.y, 2.0f);

    const auto downsampled = lcl::render::computeBackdropPassScale(
        2.0f, 100, 80, 200, 160);
    EXPECT_FLOAT_EQ(downsampled.x, 1.0f);
    EXPECT_FLOAT_EQ(downsampled.y, 1.0f);
}

TEST(LclUiTest, TransitionalBlurKeepsRealRadiusAndContinuousPassScale) {
    const auto nearSharp = lcl::render::computeBackdropBlurPlan(1.0f);
    EXPECT_FLOAT_EQ(nearSharp.gaussianValuePx, 1.0f);
    EXPECT_FLOAT_EQ(nearSharp.downsampleScale, 1.0f);

    const auto fractional = lcl::render::computeBackdropBlurPlan(12.0f);
    EXPECT_FLOAT_EQ(fractional.gaussianValuePx, 12.0f);
    EXPECT_FLOAT_EQ(fractional.downsampleScale, 1.5f);

    const auto full = lcl::render::computeBackdropBlurPlan(50.0f);
    EXPECT_FLOAT_EQ(full.gaussianValuePx, 50.0f);
    EXPECT_FLOAT_EQ(full.downsampleScale, 4.0f);
}

TEST(LclUiTest, WindowAppKeepsEffectControlsInLogicalUnits) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);

    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 64, 48, "Scaled effect graph");
    auto surface = std::make_unique<BackdropSurface>();
    surface->setWidth(64.0f);
    surface->setHeight(48.0f);
    surface->addFilter(lcl::protocol::FilterType::Blur, 8.0f);
    surface->addFilter(lcl::protocol::FilterType::Glass, 30.0f, 3.0f, 0.0f);
    app.setRootWidget(std::move(surface));
    app.setExternalIpcSocket(sockets[0]);

    lcl::protocol::LCLMsgConfigureBounds configure{};
    configure.surfaceId = app.getSurfaceId();
    configure.configureSerial = 7;
    configure.width = 64;
    configure.height = 48;
    configure.backingWidth = 64;
    configure.backingHeight = 48;
    configure.bufferScale = 2.0f;
    configure.resizeReason = lcl::protocol::LCLConfigureResizeReason::Initial;
    lcl::protocol::LCLHeader configureHeader{};
    configureHeader.opcode = lcl::protocol::LCLOpcode::ConfigureBounds;
    configureHeader.payloadSize = sizeof(configure);
    ASSERT_TRUE(lcl::protocol::sendMsgWithFd(
        sockets[1], configureHeader, &configure));

    ASSERT_TRUE(app.tick());

    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(lcl::protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, lcl::protocol::LCLOpcode::SetEffectGraph);
    ASSERT_GE(payload.size(),
              sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader) +
                  sizeof(lcl::protocol::EffectRegion) +
                  2u * sizeof(lcl::protocol::FilterOp));
    const auto* graph = reinterpret_cast<const lcl::protocol::LCLMsgSetEffectGraphHeader*>(
        payload.data());
    ASSERT_EQ(graph->regionCount, 1u);
    ASSERT_EQ(graph->filterCount, 2u);
    const auto* filters = reinterpret_cast<const lcl::protocol::FilterOp*>(
        payload.data() + sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader) +
        sizeof(lcl::protocol::EffectRegion));
    EXPECT_EQ(filters[0].type, lcl::protocol::FilterType::Blur);
    EXPECT_FLOAT_EQ(filters[0].value, 8.0f);
    EXPECT_EQ(filters[1].type, lcl::protocol::FilterType::Glass);
    EXPECT_FLOAT_EQ(filters[1].params[0], 30.0f);
    EXPECT_FLOAT_EQ(filters[1].params[1], 3.0f);
    EXPECT_FLOAT_EQ(filters[1].params[2], 0.0f);
    if (receivedFd >= 0) close(receivedFd);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LclUiTest, SoftwareGlassZeroThicknessOrRefractionLeavesPixelsUntouched) {
    const auto render = [](float thickness, float refraction) {
        std::vector<uint32_t> pixels(8 * 8);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const uint8_t r = static_cast<uint8_t>(x * 29);
                const uint8_t g = static_cast<uint8_t>(y * 31);
                const uint8_t b = static_cast<uint8_t>((x + y) * 15);
                pixels[static_cast<size_t>(y) * 8u + static_cast<size_t>(x)] =
                    0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                    (static_cast<uint32_t>(g) << 8) | b;
            }
        }
        const auto original = pixels;
        lcl::protocol::FilterOp glass{};
        glass.type = lcl::protocol::FilterType::Glass;
        glass.params[0] = thickness;
        glass.params[1] = refraction;
        glass.params[2] = 7.0f;
        lcl::render::RasterRenderer renderer;
        EXPECT_TRUE(renderer.initialize(8, 8, nullptr, pixels.data()));
        renderer.applyBackdropFilter(0, 0, 8, 8, 0.0f, 2.0f, 1.0f, {glass});
        return std::pair{std::move(pixels), original};
    };

    const auto [zeroThickness, originalThickness] = render(0.0f, 3.0f);
    EXPECT_EQ(zeroThickness, originalThickness);
    const auto [zeroRefraction, originalRefraction] = render(6.0f, 0.0f);
    EXPECT_EQ(zeroRefraction, originalRefraction);
}

TEST(LclUiTest, SoftwareGlassZeroDispersionDoesNotSplitColorChannels) {
    std::vector<uint32_t> pixels(16 * 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const uint8_t gray = static_cast<uint8_t>((x * 11 + y * 7) & 0xFF);
            pixels[static_cast<size_t>(y) * 16u + static_cast<size_t>(x)] =
                0xFF000000u | (static_cast<uint32_t>(gray) << 16) |
                (static_cast<uint32_t>(gray) << 8) | gray;
        }
    }

    lcl::protocol::FilterOp glass{};
    glass.type = lcl::protocol::FilterType::Glass;
    glass.params[0] = 6.0f;
    glass.params[1] = 3.0f;
    glass.params[2] = 0.0f;
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(16, 16, nullptr, pixels.data()));
    renderer.applyBackdropFilter(0, 0, 16, 16, 0.0f, 2.0f, 1.0f, {glass});

    for (uint32_t pixel : pixels) {
        const uint8_t r = static_cast<uint8_t>((pixel >> 16) & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((pixel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>(pixel & 0xFFu);
        EXPECT_EQ(r, g);
        EXPECT_EQ(g, b);
    }
}

TEST(LclUiTest, PassiveBackdropEffectDoesNotRequireAFullWindowRoundedRaster) {
    RecordingCanvas canvas;
    auto root = std::make_unique<Container>();
    root->setBackgroundColor({17, 19, 23, 184});
    root->setWidth(540.0f);
    root->setHeight(360.0f);

    auto effect = std::make_unique<BackdropSurface>();
    effect->setInteractive(false);
    effect->setBorderRadius(20.0f);
    effect->setBorderRoundness(3.2f);
    effect->addFilter(lcl::protocol::FilterType::Blur, 3.5f);
    effect->setPositionType(layout::PositionType::Absolute);
    effect->setWidth(540.0f);
    effect->setHeight(360.0f);
    root->addChild(std::move(effect));

    root->calculateLayout(540.0f, 360.0f);
    root->syncLayout();
    root->draw(canvas, {0.0f, 0.0f, 540.0f, 360.0f});

    EXPECT_EQ(canvas.rects.size(), 1u);
    EXPECT_TRUE(canvas.roundedRects.empty());

    std::vector<EffectRegion> effects;
    root->collectEffects(effects);
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_EQ(effects.front().source, EffectSource::SurfaceBackdrop);
    EXPECT_EQ(effects.front().cornerRadius, 20.0f);
    EXPECT_FLOAT_EQ(effects.front().cornerRoundness, 3.2f);
}

TEST(LclUiTest, AnyWidgetRecordsBackdropInDisplayListButSendsOnlySurfaceEffects) {
    lcl::render::RasterCanvas canvas(true);
    ASSERT_TRUE(canvas.initialize(160, 80, nullptr));

    Button button("Blurred");
    button.setWidth(120.0f);
    button.setHeight(40.0f);
    button.setBorderRadius(12.0f);
    button.addEffect(EffectSource::Backdrop,
                     {lcl::protocol::FilterType::Blur, 10.0f});
    button.calculateLayout(160.0f, 80.0f);
    button.syncLayout();

    canvas.beginFrame();
    button.draw(canvas, {0.0f, 0.0f, 160.0f, 80.0f});
    canvas.endFrame();
    const auto frame = canvas.takeDisplayListFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(std::any_of(
        frame->displayList.commands().begin(), frame->displayList.commands().end(),
        [](const auto& command) {
            return std::holds_alternative<graphics::ApplyBackdropEffectsCommand>(command);
        }));

    std::vector<EffectRegion> effects;
    button.collectEffects(effects);
    EXPECT_TRUE(effects.empty());

    button.addEffect(EffectSource::SurfaceBackdrop,
                     {lcl::protocol::FilterType::Blur, 10.0f});
    button.collectEffects(effects);
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_EQ(effects.front().source, EffectSource::SurfaceBackdrop);
}

TEST(LclUiTest, RasterCanvasInjectionPreservesRasterOutput) {
    WindowApp app(lcl::render::makeRasterCanvas(), 8, 8, "LCL raster graphics::Canvas Test");
    auto root = std::make_unique<Container>();
    root->setBackgroundColor({11, 22, 33, 255});
    root->setWidth(8.0f);
    root->setHeight(8.0f);
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_NE(app.getPixelBuffer(), nullptr);
    EXPECT_EQ(app.getPixelBuffer()[0], 0xFF0B1621u);
}

TEST(LclUiTest, RenderPassDamageRect) {
    RenderPass pass;
    EXPECT_FALSE(pass.hasDamage());

    pass.addDirtyRect(graphics::RectF{10.0f, 10.0f, 20.0f, 20.0f});
    pass.addDirtyRect(graphics::RectF{50.0f, 50.0f, 20.0f, 20.0f});

    EXPECT_TRUE(pass.hasDamage());
    EXPECT_EQ(pass.getDirtyRects().size(), 2u);
    graphics::RectF damage = pass.getDamageRect();
    EXPECT_EQ(damage.x, 10.0f);
    EXPECT_EQ(damage.y, 10.0f);
    EXPECT_EQ(damage.width, 60.0f);
    EXPECT_EQ(damage.height, 60.0f);

    pass.clear();
    EXPECT_FALSE(pass.hasDamage());
}

TEST(LclUiTest, RenderPassMergesOverlappingDamageButKeepsDistantRegions) {
    RenderPass pass;
    pass.addDirtyRect({10.0f, 10.0f, 20.0f, 20.0f});
    pass.addDirtyRect({15.0f, 15.0f, 20.0f, 20.0f});
    pass.addDirtyRect({80.0f, 60.0f, 10.0f, 10.0f});

    ASSERT_EQ(pass.getDirtyRects().size(), 2u);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().x, 10.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().y, 10.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().width, 25.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().height, 25.0f);
}

TEST(LclUiTest, ChildPaintInvalidationDoesNotExpandDamageToRootBounds) {
    RenderPass pass;
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(120.0f);
    auto child = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = child.get();
    child->setWidth(30.0f);
    child->setHeight(20.0f);
    root->addChild(std::move(child));
    root->calculateLayout(200.0f, 120.0f);
    root->syncLayout();
    root->setRenderPass(&pass);
    pass.clear();

    const uint64_t rootRevision = root->getPaintRevision();
    childPtr->invalidatePaint();

    ASSERT_EQ(pass.getDirtyRects().size(), 1u);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().width, 30.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().height, 20.0f);
    EXPECT_GT(root->getPaintRevision(), rootRevision);
}

TEST(LclUiTest, RemovingChildDamagesItsOldBoundsWithoutExpandingToRoot) {
    RenderPass pass;
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(120.0f);
    auto child = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = child.get();
    child->setWidth(30.0f);
    child->setHeight(20.0f);
    root->addChild(std::move(child));
    root->calculateLayout(200.0f, 120.0f);
    root->syncLayout();
    root->setRenderPass(&pass);
    pass.clear();
    const uint64_t layoutRevision = root->getLayoutRevision();
    const uint64_t paintRevision = root->getPaintRevision();

    root->removeChild(childPtr);

    ASSERT_EQ(pass.getDirtyRects().size(), 1u);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().width, 30.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().height, 20.0f);
    EXPECT_GT(root->getLayoutRevision(), layoutRevision);
    EXPECT_GT(root->getPaintRevision(), paintRevision);
}

TEST(LclUiTest, ExplicitWidgetPaintDamageKeepsTheRequestedSubregion) {
    RenderPass pass;
    auto widget = std::make_unique<CountingPaintWidget>();
    widget->setWidth(100.0f);
    widget->setHeight(80.0f);
    widget->calculateLayout(100.0f, 80.0f);
    widget->syncLayout();
    widget->setRenderPass(&pass);
    pass.clear();

    const uint64_t revision = widget->getPaintRevision();
    widget->invalidatePaint({12.0f, 18.0f, 9.0f, 15.0f});

    ASSERT_EQ(pass.getDirtyRects().size(), 1u);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().x, 12.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().y, 18.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().width, 9.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().height, 15.0f);
    EXPECT_GT(widget->getPaintRevision(), revision);
}

TEST(LclUiTest, WindowAppRetainedFrameAddsRasterCoverageToChangedWidgetRegion) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 200, 120, "Retained damage");
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(120.0f);
    auto child = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = child.get();
    child->setWidth(30.0f);
    child->setHeight(20.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    recorded->clearedRects.clear();
    childPtr->invalidatePaint();
    ASSERT_TRUE(app.renderFrame());

    ASSERT_EQ(recorded->clearedRects.size(), 1u);
    EXPECT_FLOAT_EQ(recorded->clearedRects.front().x, 0.0f);
    EXPECT_FLOAT_EQ(recorded->clearedRects.front().y, 0.0f);
    EXPECT_FLOAT_EQ(recorded->clearedRects.front().width, 31.0f);
    EXPECT_FLOAT_EQ(recorded->clearedRects.front().height, 21.0f);
}

TEST(LclUiTest, DisplayListTransportRecordsOnlyTheDamagedPatchAfterFirstFrame) {
    auto canvas = std::make_unique<lcl::render::RasterCanvas>(true);
    auto* recorded = canvas.get();
    WindowApp app(std::move(canvas), 200, 120, "Retained DisplayList patch");
    auto root = std::make_unique<Container>();
    auto child = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = child.get();
    child->setWidth(30.0f);
    child->setHeight(20.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    childPtr->invalidatePaint();
    ASSERT_TRUE(app.renderFrame());

    const auto& commands = recorded->lastDisplayList().commands();
    ASSERT_FALSE(commands.empty());
    const auto* clear = std::get_if<graphics::ClearRectCommand>(&commands.front());
    ASSERT_NE(clear, nullptr);
    EXPECT_LT(clear->rect.width, 200.0f);
    EXPECT_LT(clear->rect.height, 120.0f);
}

TEST(LclUiTest, WindowAppLayoutChangeDamagesChangedWidgetsNotFullSurface) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 200, 120, "Retained layout damage");

    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(120.0f);
    root->setDirection(layout::Direction::Column);
    root->setAlignItems(layout::Align::FlexStart);
    auto label = std::make_unique<Text>("i");
    Text* labelPtr = label.get();
    root->addChild(std::move(label));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    recorded->clearedRects.clear();
    labelPtr->setText("WWWWWWWW");
    ASSERT_TRUE(app.renderFrame());

    ASSERT_FALSE(recorded->clearedRects.empty());
    for (const graphics::RectF& cleared : recorded->clearedRects) {
        EXPECT_LT(cleared.height, 120.0f);
    }
}

TEST(LclUiTest, RasterRendererClearRectReplacesOnlyRequestedRetainedPixels) {
    std::vector<uint32_t> pixels(8 * 8, 0xFF123456u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(8, 8, nullptr, pixels.data()));

    renderer.clearRect({2.0f, 2.0f, 3.0f, 3.0f}, {0, 0, 0, 0});

    EXPECT_EQ(pixels[0], 0xFF123456u);
    EXPECT_EQ(pixels[2 + 2 * 8], 0x00000000u);
    EXPECT_EQ(pixels[4 + 4 * 8], 0x00000000u);
    EXPECT_EQ(pixels[5 + 5 * 8], 0xFF123456u);
}

TEST(LclUiTest, RasterRendererCopiesACompositedRegionIntoOwnedPixels) {
    std::vector<uint32_t> pixels{
        0xFF000001u, 0xFF000002u, 0xFF000003u, 0xFF000004u,
        0xFF000005u, 0xFF000006u, 0xFF000007u, 0xFF000008u,
        0xFF000009u, 0xFF00000Au, 0xFF00000Bu, 0xFF00000Cu,
        0xFF00000Du, 0xFF00000Eu, 0xFF00000Fu, 0xFF000010u,
    };
    std::vector<uint32_t> retained(4, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(4, 4, nullptr, pixels.data()));

    ASSERT_TRUE(renderer.copyFrameRegionToCachedLayer(
        0, 0, retained.data(), 2, 2, {1.0f, 1.0f, 2.0f, 2.0f}));

    EXPECT_EQ(retained[0], 0xFF000006u);
    EXPECT_EQ(retained[1], 0xFF000007u);
    EXPECT_EQ(retained[2], 0xFF00000Au);
    EXPECT_EQ(retained[3], 0xFF00000Bu);
}

TEST(LclUiTest, RasterRendererPatchesOnlyDamagedPixelsInOwnedCache) {
    std::vector<uint32_t> pixels{
        0xFF000001u, 0xFF000002u, 0xFF000003u, 0xFF000004u,
        0xFF000005u, 0xFF000006u, 0xFF000007u, 0xFF000008u,
        0xFF000009u, 0xFF00000Au, 0xFF00000Bu, 0xFF00000Cu,
        0xFF00000Du, 0xFF00000Eu, 0xFF00000Fu, 0xFF000010u,
    };
    std::vector<uint32_t> retained(4, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(4, 4, nullptr, pixels.data()));
    ASSERT_TRUE(renderer.copyFrameRegionToCachedLayer(
        0, 0, retained.data(), 2, 2, {1.0f, 1.0f, 2.0f, 2.0f}));

    pixels[2 + 2 * 4] = 0xFFABCDEFu;
    ASSERT_TRUE(renderer.copyFrameDamageToCachedLayer(
        0, 0, retained.data(), 2, 2,
        {1.0f, 1.0f, 2.0f, 2.0f},
        {2.0f, 2.0f, 1.0f, 1.0f}));

    EXPECT_EQ(retained[0], 0xFF000006u);
    EXPECT_EQ(retained[1], 0xFF000007u);
    EXPECT_EQ(retained[2], 0xFF00000Au);
    EXPECT_EQ(retained[3], 0xFFABCDEFu);
}

TEST(LclUiTest, RasterRendererRetainedModeDoesNotClearUnchangedFramePixels) {
    std::vector<uint32_t> pixels(4 * 4, 0xFFABCDEFu);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(4, 4, nullptr, pixels.data()));

    renderer.setRetainsFrameBacking(true);
    renderer.beginFrame();
    EXPECT_EQ(pixels[0], 0xFFABCDEFu);

    renderer.setRetainsFrameBacking(false);
    renderer.beginFrame();
    EXPECT_EQ(pixels[0], 0xFF14161Du);
}

TEST(LclUiTest, RasterRendererAppliesDisplayListPatchOverRetainedScene) {
    std::vector<uint32_t> pixels(8 * 8, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(8, 8, nullptr, pixels.data()));
    renderer.setRetainsFrameBacking(true);
    const graphics::RenderTarget target{{8.0f, 8.0f}, {8, 8}, 1.0f};

    graphics::DisplayListBuilder full;
    full.clearRect({0.0f, 0.0f, 8.0f, 8.0f}, {0, 0, 0, 0});
    graphics::Path fullPath;
    fullPath.addRect({0.0f, 0.0f, 8.0f, 8.0f});
    full.drawPath(fullPath, graphics::Paint{{255, 0, 0, 255}});
    renderer.beginFrame();
    renderer.replayDisplayList(full.build(), target);
    renderer.endFrame();

    graphics::DisplayListBuilder patch;
    patch.clearRect({2.0f, 2.0f, 2.0f, 2.0f}, {0, 0, 0, 0});
    graphics::Path patchPath;
    patchPath.addRect({2.0f, 2.0f, 2.0f, 2.0f});
    patch.drawPath(patchPath, graphics::Paint{{0, 0, 255, 255}});
    renderer.beginFrame();
    renderer.setFrameDamageRect(
        lcl::render::RasterRect{2.0f, 2.0f, 2.0f, 2.0f});
    renderer.replayDisplayList(patch.build(), target);
    renderer.endFrame();

    EXPECT_EQ(pixels[0], 0xFFFF0000u);
    EXPECT_EQ(pixels[2 + 2 * 8], 0xFF0000FFu);
    EXPECT_EQ(pixels[7 + 7 * 8], 0xFFFF0000u);
}

TEST(LclUiTest, RasterRendererTransformsCachedLayerAtCompositionTime) {
    std::vector<uint32_t> pixels(8 * 8, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(8, 8, nullptr, pixels.data()));
    const graphics::RenderTarget target{{8.0f, 8.0f}, {8, 8}, 1.0f};

    graphics::DisplayListBuilder frame;
    frame.clearRect({0.0f, 0.0f, 8.0f, 8.0f}, {0, 0, 0, 0});
    frame.beginCachedLayer(91, {0.0f, 0.0f, 2.0f, 2.0f});
    graphics::Path layerPath;
    layerPath.addRect({0.0f, 0.0f, 2.0f, 2.0f});
    frame.drawPath(layerPath, graphics::Paint{{255, 0, 0, 255}});
    frame.endCachedLayer();
    frame.drawCachedLayerTransformed(
        91, {0.0f, 0.0f, 2.0f, 2.0f}, 1.0f,
        graphics::Matrix3::translation(3.0f, 2.0f));

    renderer.beginFrame();
    renderer.replayDisplayList(frame.build(), target);
    renderer.endFrame();

    EXPECT_EQ(pixels[0], 0x00000000u);
    EXPECT_EQ(pixels[3 + 2 * 8], 0xFFFF0000u);
    EXPECT_EQ(pixels[4 + 3 * 8], 0xFFFF0000u);
}

TEST(LclUiTest, OpaquePresentationLayersPreserveNestedGroupOpacityAndClipping) {
    std::vector<uint32_t> pixels(8 * 8, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(8, 8, nullptr, pixels.data()));
    graphics::DisplayListBuilder frame;
    frame.clearRect({0, 0, 8, 8}, {255, 255, 255, 255});
    frame.beginLayer(1.0f);
    frame.clipRect({2, 2, 4, 4});
    frame.concat(graphics::Matrix3::translation(2, 2));
    graphics::Path rect;
    rect.addRect({0, 0, 4, 4});
    frame.drawPath(rect, graphics::Paint{{255, 0, 0, 255}});
    frame.beginLayer(0.5f);
    frame.beginLayer(1.0f);
    // These overlapping draws must receive group alpha once, not per draw.
    frame.drawPath(rect, graphics::Paint{{0, 0, 255, 255}});
    frame.drawPath(rect, graphics::Paint{{0, 0, 255, 255}});
    frame.endLayer();
    frame.endLayer();
    frame.endLayer();
    renderer.beginFrame();
    ASSERT_TRUE(renderer.replayDisplayList(
        frame.build(), {{8, 8}, {8, 8}, 1.0f}));
    renderer.endFrame();
    EXPECT_EQ(pixels[0], 0xFFFFFFFFu);
    EXPECT_EQ(pixels[6 + 6 * 8], 0xFFFFFFFFu);
    const auto center = pixels[3 + 3 * 8];
    EXPECT_NEAR((center >> 16) & 255u, 127, 1);
    EXPECT_EQ((center >> 8) & 255u, 0u);
    EXPECT_NEAR(center & 255u, 128, 1);
    EXPECT_EQ(center >> 24, 255u);
}

TEST(LclUiTest, OpaquePresentationLayerClearKeepsItsIsolatedDestination) {
    std::vector<uint32_t> pixels(8 * 8, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(8, 8, nullptr, pixels.data()));
    graphics::DisplayListBuilder frame;
    frame.clearRect({0, 0, 8, 8}, {255, 255, 255, 255});
    frame.beginLayer(1.0f);
    graphics::Path rect;
    rect.addRect({0, 0, 8, 8});
    frame.drawPath(rect, graphics::Paint{{255, 0, 0, 255}});
    frame.clearRect({2, 2, 4, 4}, {0, 0, 0, 0});
    frame.endLayer();
    renderer.beginFrame();
    ASSERT_TRUE(renderer.replayDisplayList(
        frame.build(), {{8, 8}, {8, 8}, 1.0f}));
    renderer.endFrame();
    EXPECT_EQ(pixels[0], 0xFFFF0000u);
    EXPECT_EQ(pixels[3 + 3 * 8], 0xFFFFFFFFu);
}

TEST(LclUiTest, OpaquePresentationLayerMatchesIsolatedAntialiasedClip) {
    const auto paint = [](bool forceIsolation, bool rounded) {
        std::vector<uint32_t> pixels(16 * 16, 0u);
        lcl::render::RasterRenderer renderer;
        if (!renderer.initialize(16, 16, nullptr, pixels.data())) return pixels;
        graphics::DisplayListBuilder frame;
        frame.clearRect({0, 0, 16, 16}, {255, 255, 255, 255});
        if (rounded) {
            graphics::Path clip;
            clip.addRRect({{1.25f, 1.25f, 12.5f, 12.5f}, 3, 3, 2});
            frame.clipPath(clip);
        } else {
            frame.clipRect({1.25f, 1.25f, 12.5f, 12.5f});
        }
        frame.beginLayer(1.0f);
        if (forceIsolation) frame.clearRect({0, 0, 16, 16}, {0, 0, 0, 0});
        graphics::Path rect;
        rect.addRect({0, 0, 16, 16});
        frame.drawPath(rect, graphics::Paint{{255, 0, 0, 160}});
        frame.drawPath(rect, graphics::Paint{{0, 0, 255, 160}});
        frame.endLayer();
        renderer.beginFrame();
        EXPECT_TRUE(renderer.replayDisplayList(
            frame.build(), {{16, 16}, {16, 16}, 1.0f}));
        renderer.endFrame();
        return pixels;
    };
    EXPECT_EQ(paint(false, true), paint(true, true));
    EXPECT_EQ(paint(false, false), paint(true, false));
}

TEST(LclUiTest, PresentationMotionDoesNotInvalidateAncestorPaintCacheRevision) {
    MotionCoordinator coordinator;
    auto root = std::make_unique<Container>();
    auto child = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = child.get();
    root->addChild(std::move(child));
    root->setMotionCoordinator(&coordinator);
    const uint64_t paintRevision = root->getPaintRevision();
    const uint64_t presentationRevision = root->getPresentationRevision();

    coordinator.animateFloat(
        *childPtr, AnimatableProperty::ScaleX, 1.0f, 0.9f,
        Motion::tween(0.2f, Easing::linear()),
        [childPtr](float value) {
            childPtr->applyPresentationValue(AnimatableProperty::ScaleX, value);
        });
    coordinator.tick(0.05f);

    EXPECT_EQ(root->getPaintRevision(), paintRevision);
    EXPECT_GT(root->getPresentationRevision(), presentationRevision);
}

TEST(LclUiTest, PresentationTransformDamagesOldAndNewBoundsWithoutPaintInvalidation) {
    RenderPass pass;
    auto widget = std::make_unique<CountingPaintWidget>();
    widget->setWidth(30.0f);
    widget->setHeight(20.0f);
    widget->calculateLayout(200.0f, 120.0f);
    widget->syncLayout();
    widget->setRenderPass(&pass);
    pass.clear();
    const uint64_t paintRevision = widget->getPaintRevision();

    widget->setTranslationX(100.0f);

    ASSERT_EQ(pass.getDirtyRects().size(), 2u);
    EXPECT_FLOAT_EQ(pass.getDirtyRects()[0].x, 0.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects()[0].width, 30.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects()[1].x, 100.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects()[1].width, 30.0f);
    EXPECT_EQ(widget->getPaintRevision(), paintRevision);
    EXPECT_GT(widget->getPresentationRevision(), 0u);
}

TEST(LclUiTest, NoOpPresentationSetterDoesNotAdvanceRevisionOrDamage) {
    RenderPass pass;
    auto widget = std::make_unique<CountingPaintWidget>();
    widget->setWidth(30.0f);
    widget->setHeight(20.0f);
    widget->calculateLayout(30.0f, 20.0f);
    widget->syncLayout();
    widget->setRenderPass(&pass);
    pass.clear();
    const uint64_t revision = widget->getPresentationRevision();

    widget->setTranslationX(0.0f);

    EXPECT_EQ(widget->getPresentationRevision(), revision);
    EXPECT_FALSE(pass.hasDamage());
}

TEST(LclUiTest, LayoutInvalidationIsIndependentUntilGeometryIsSynchronized) {
    WindowApp app(std::make_unique<RecordingCanvas>(), 200, 120,
                  "Independent layout revision");
    auto root = std::make_unique<Container>();
    root->setDirection(layout::Direction::Row);
    auto child = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = child.get();
    child->setWidth(30.0f);
    child->setHeight(20.0f);
    root->addChild(std::move(child));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());

    const uint64_t layoutRevision = childPtr->getLayoutRevision();
    const uint64_t paintRevision = childPtr->getPaintRevision();
    const uint64_t presentationRevision = childPtr->getPresentationRevision();
    childPtr->setWidth(50.0f);

    EXPECT_GT(childPtr->getLayoutRevision(), layoutRevision);
    EXPECT_EQ(childPtr->getPaintRevision(), paintRevision);
    EXPECT_EQ(childPtr->getPresentationRevision(), presentationRevision);
    EXPECT_TRUE(childPtr->isLayoutDirty());

    app.updateLayout();
    EXPECT_FALSE(childPtr->isLayoutDirty());
    EXPECT_EQ(childPtr->getPaintRevision(), paintRevision);
    EXPECT_GT(childPtr->getPresentationRevision(), presentationRevision);
}

TEST(LclUiTest, LayoutResolvingToSameGeometryDoesNotCreatePresentationWork) {
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(120.0f);
    auto child = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = child.get();
    child->setWidth(30.0f);
    child->setHeight(20.0f);
    root->addChild(std::move(child));
    root->calculateLayout(200.0f, 120.0f);
    root->syncLayout();

    const uint64_t layoutRevision = childPtr->getLayoutRevision();
    const uint64_t paintRevision = childPtr->getPaintRevision();
    const uint64_t presentationRevision = childPtr->getPresentationRevision();
    childPtr->setHeight(20.0f);

    EXPECT_EQ(childPtr->getLayoutRevision(), layoutRevision);
    EXPECT_EQ(childPtr->getPaintRevision(), paintRevision);
    EXPECT_EQ(childPtr->getPresentationRevision(), presentationRevision);
    EXPECT_FALSE(childPtr->isLayoutDirty());
}

TEST(LclUiTest, VisibilityAndClippingInvalidatePresentationOnly) {
    RenderPass pass;
    auto widget = std::make_unique<CountingPaintWidget>();
    widget->setWidth(30.0f);
    widget->setHeight(20.0f);
    widget->calculateLayout(30.0f, 20.0f);
    widget->syncLayout();
    widget->setRenderPass(&pass);
    pass.clear();
    const uint64_t paintRevision = widget->getPaintRevision();
    const uint64_t presentationRevision = widget->getPresentationRevision();

    widget->setVisible(false);
    ASSERT_EQ(pass.getDirtyRects().size(), 1u);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().width, 30.0f);
    EXPECT_FLOAT_EQ(pass.getDirtyRects().front().height, 20.0f);
    pass.clear();

    widget->setVisible(true);
    ASSERT_EQ(pass.getDirtyRects().size(), 1u);
    pass.clear();
    widget->setClipsToBounds(true);

    EXPECT_EQ(widget->getPaintRevision(), paintRevision);
    EXPECT_GT(widget->getPresentationRevision(), presentationRevision);
    EXPECT_TRUE(pass.hasDamage());
}

TEST(LclUiTest, CollapsedWidgetLeavesLayoutWhileHiddenWidgetKeepsItsSpace) {
    auto root = std::make_unique<Container>();
    root->setWidth(200.0f);
    root->setHeight(40.0f);
    root->setDirection(layout::Direction::Row);

    auto first = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* firstPtr = first.get();
    first->setWidth(80.0f);
    first->setHeight(40.0f);
    root->addChild(std::move(first));

    auto second = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* secondPtr = second.get();
    second->setWidth(40.0f);
    second->setHeight(40.0f);
    root->addChild(std::move(second));

    root->calculateLayout(200.0f, 40.0f);
    root->syncLayout();
    EXPECT_FLOAT_EQ(secondPtr->getAbsoluteBounds().x, 80.0f);

    // The established visibility API hides paint only, so it still reserves
    // the first item's 80 logical pixels.
    firstPtr->setVisible(false);
    root->calculateLayout(200.0f, 40.0f);
    root->syncLayout();
    EXPECT_FLOAT_EQ(secondPtr->getAbsoluteBounds().x, 80.0f);

    firstPtr->setLayoutVisibility(LayoutVisibility::Collapsed);
    root->calculateLayout(200.0f, 40.0f);
    root->syncLayout();
    EXPECT_TRUE(firstPtr->isCollapsed());
    EXPECT_TRUE(firstPtr->getVisiblePresentationPaintBounds().isEmpty());
    EXPECT_FLOAT_EQ(firstPtr->getBounds().width, 0.0f);
    EXPECT_FLOAT_EQ(secondPtr->getAbsoluteBounds().x, 0.0f);

    firstPtr->setVisible(true);
    firstPtr->setLayoutVisibility(LayoutVisibility::Visible);
    root->calculateLayout(200.0f, 40.0f);
    root->syncLayout();
    EXPECT_FALSE(firstPtr->isCollapsed());
    EXPECT_FLOAT_EQ(secondPtr->getAbsoluteBounds().x, 80.0f);
}

TEST(LclUiTest, LayoutEnvironmentTracksSafeAreaAndWidthDerivedSizeClass) {
    auto canvas = std::make_unique<RecordingCanvas>();
    WindowApp app(std::move(canvas), 760.0f, 600.0f, "Layout environment");
    app.setLayoutSizeClassPolicy({.minimumSidebarWidth = 280.0f,
                                  .minimumDetailWidth = 480.0f});

    const LayoutEnvironment initial = app.getLayoutEnvironment();
    EXPECT_FLOAT_EQ(initial.availableWidth, 760.0f);
    EXPECT_FLOAT_EQ(initial.availableHeight, 600.0f);
    EXPECT_EQ(initial.sizeClass, LayoutSizeClass::Expanded);

    std::vector<LayoutEnvironment> changes;
    app.setOnLayoutEnvironmentChanged(
        [&](const LayoutEnvironment& environment) { changes.push_back(environment); });

    app.setSafeAreaInsets({.top = 24.0f, .right = 16.0f, .bottom = 20.0f, .left = 8.0f});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_FLOAT_EQ(changes.back().availableWidth, 736.0f);
    EXPECT_FLOAT_EQ(changes.back().availableHeight, 556.0f);
    EXPECT_EQ(changes.back().sizeClass, LayoutSizeClass::Compact);

    app.resize(784.0f, 600.0f);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_FLOAT_EQ(changes.back().availableWidth, 760.0f);
    EXPECT_EQ(changes.back().sizeClass, LayoutSizeClass::Expanded);

    // Negative and non-finite inset values cannot manufacture extra space.
    app.setSafeAreaInsets({.top = -1.0f,
                           .right = std::numeric_limits<float>::infinity(),
                           .bottom = 0.0f,
                           .left = -4.0f});
    ASSERT_EQ(changes.size(), 3u);
    EXPECT_FLOAT_EQ(changes.back().safeArea.top, 0.0f);
    EXPECT_FLOAT_EQ(changes.back().safeArea.right, 0.0f);
    EXPECT_FLOAT_EQ(changes.back().availableWidth, 784.0f);
    EXPECT_EQ(changes.back().sizeClass, LayoutSizeClass::Expanded);
}

TEST(LclUiTest, TextContentExplicitlyInvalidatesLayoutAndPaint) {
    Text text("short");
    text.calculateLayout();
    text.syncLayout();
    const uint64_t layoutRevision = text.getLayoutRevision();
    const uint64_t paintRevision = text.getPaintRevision();
    const uint64_t presentationRevision = text.getPresentationRevision();

    text.setText("a longer label");

    EXPECT_GT(text.getLayoutRevision(), layoutRevision);
    EXPECT_GT(text.getPaintRevision(), paintRevision);
    EXPECT_EQ(text.getPresentationRevision(), presentationRevision);
}

TEST(LclUiTest, PaintStyleDoesNotScheduleLayoutOrPresentation) {
    Text text("label");
    text.calculateLayout();
    text.syncLayout();
    const uint64_t layoutRevision = text.getLayoutRevision();
    const uint64_t paintRevision = text.getPaintRevision();
    const uint64_t presentationRevision = text.getPresentationRevision();

    text.setTextColor({12, 34, 56, 255});

    EXPECT_EQ(text.getLayoutRevision(), layoutRevision);
    EXPECT_GT(text.getPaintRevision(), paintRevision);
    EXPECT_EQ(text.getPresentationRevision(), presentationRevision);
}

TEST(LclUiTest, NormalLayoutDamageDoesNotExpandToTheFullWindow) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 200, 120, "Scoped layout damage");
    auto root = std::make_unique<Container>();
    root->setDirection(layout::Direction::Row);
    auto first = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* firstPtr = first.get();
    first->setWidth(30.0f);
    first->setHeight(20.0f);
    root->addChild(std::move(first));
    auto second = std::make_unique<CountingPaintWidget>();
    second->setWidth(30.0f);
    second->setHeight(20.0f);
    root->addChild(std::move(second));
    app.setRootWidget(std::move(root));
    ASSERT_TRUE(app.renderFrame());
    recorded->clearedRects.clear();

    firstPtr->setWidth(50.0f);
    ASSERT_TRUE(app.renderFrame());

    ASSERT_FALSE(recorded->clearedRects.empty());
    for (const graphics::RectF& rect : recorded->clearedRects) {
        EXPECT_LT(rect.width, 200.0f);
        EXPECT_LT(rect.height, 120.0f);
    }
}

TEST(LclUiTest, LayoutFlexTreeUsesPublicWidgetApi) {
    auto root = std::make_unique<Container>();
    root->setDirection(layout::Direction::Column);
    root->setWidth(200.0f);
    root->setHeight(400.0f);
    root->setPadding(10.0f);

    auto child1 = std::make_unique<Widget>();
    Widget* child1Ptr = child1.get();
    child1->setHeight(50.0f);
    root->addChild(std::move(child1));

    auto child2 = std::make_unique<Widget>();
    Widget* child2Ptr = child2.get();
    child2->setFlexGrow(1.0f);
    root->addChild(std::move(child2));

    root->calculateLayout(200.0f, 400.0f);
    root->syncLayout();

    EXPECT_EQ(child1Ptr->getBounds().y, 10.0f);
    EXPECT_EQ(child1Ptr->getBounds().height, 50.0f);

    EXPECT_EQ(child2Ptr->getBounds().y, 60.0f);
    EXPECT_EQ(child2Ptr->getBounds().height, 330.0f); // 400 - 20 (padding) - 50 = 330
}

TEST(LclUiTest, LayoutSupportsReverseWrapAbsoluteAndSizingConstraints) {
    auto root = std::make_unique<Container>();
    root->setDirection(layout::Direction::RowReverse);
    root->setWrap(layout::Wrap::Wrap);
    root->setJustifyContent(layout::Justify::FlexStart);
    root->setAlignItems(layout::Align::Center);
    root->setWidth(120.0f);
    root->setHeight(90.0f);
    root->setPadding(layout::Edge::Horizontal, 10.0f);
    root->setGap(layout::Gutter::Column, 5.0f);

    auto first = std::make_unique<Widget>();
    Widget* firstPtr = first.get();
    first->setWidth(50.0f);
    first->setHeight(20.0f);
    first->setMinWidth(40.0f);
    first->setMaxWidth(60.0f);
    root->addChild(std::move(first));

    auto second = std::make_unique<Widget>();
    Widget* secondPtr = second.get();
    second->setFlexBasis(50.0f);
    second->setFlexShrink(0.0f);
    second->setHeight(20.0f);
    second->setMargin(layout::Edge::Left, 5.0f);
    root->addChild(std::move(second));

    auto absolute = std::make_unique<Widget>();
    Widget* absolutePtr = absolute.get();
    absolute->setPositionType(layout::PositionType::Absolute);
    absolute->setPosition(layout::Edge::Left, 7.0f);
    absolute->setPosition(layout::Edge::Top, 9.0f);
    absolute->setWidth(11.0f);
    absolute->setHeight(13.0f);
    root->addChild(std::move(absolute));

    root->calculateLayout(120.0f, 90.0f);
    root->syncLayout();

    EXPECT_GT(firstPtr->getBounds().x, secondPtr->getBounds().x);
    EXPECT_EQ(absolutePtr->getBounds().x, 7.0f);
    EXPECT_EQ(absolutePtr->getBounds().y, 9.0f);
    EXPECT_EQ(absolutePtr->getBounds().width, 11.0f);
    EXPECT_EQ(absolutePtr->getBounds().height, 13.0f);

    root->setDirection(layout::Direction::ColumnReverse);
    root->setWrap(layout::Wrap::NoWrap);
    root->calculateLayout(120.0f, 90.0f);
    root->syncLayout();
    EXPECT_GT(firstPtr->getBounds().y, secondPtr->getBounds().y);

    firstPtr->setWidthAuto();
    firstPtr->setFlexBasisAuto();
    root->calculateLayout(120.0f, 90.0f);
    root->syncLayout();
    EXPECT_GE(firstPtr->getBounds().width, 40.0f);
    EXPECT_LE(firstPtr->getBounds().width, 60.0f);
}

TEST(LclUiTest, LayoutDistinguishesStaticRelativeAndAbsolutePositioning) {
    auto root = std::make_unique<Container>();
    root->setDirection(layout::Direction::Row);
    root->setWidth(100.0f);
    root->setHeight(40.0f);

    auto staticChild = std::make_unique<Widget>();
    Widget* staticPtr = staticChild.get();
    staticChild->setPositionType(layout::PositionType::Static);
    staticChild->setPosition(layout::Edge::Left, 30.0f);
    staticChild->setWidth(20.0f);
    staticChild->setHeight(10.0f);
    root->addChild(std::move(staticChild));

    auto relativeChild = std::make_unique<Widget>();
    Widget* relativePtr = relativeChild.get();
    relativeChild->setPositionType(layout::PositionType::Relative);
    relativeChild->setPosition(layout::Edge::Left, 30.0f);
    relativeChild->setWidth(20.0f);
    relativeChild->setHeight(10.0f);
    root->addChild(std::move(relativeChild));

    auto absoluteChild = std::make_unique<Widget>();
    Widget* absolutePtr = absoluteChild.get();
    absoluteChild->setPositionType(layout::PositionType::Absolute);
    absoluteChild->setPosition(layout::Edge::Right, 5.0f);
    absoluteChild->setPosition(layout::Edge::Bottom, 6.0f);
    absoluteChild->setWidth(10.0f);
    absoluteChild->setHeight(8.0f);
    root->addChild(std::move(absoluteChild));

    root->calculateLayout(100.0f, 40.0f);
    root->syncLayout();
    EXPECT_EQ(staticPtr->getBounds().x, 0.0f);
    EXPECT_EQ(relativePtr->getBounds().x, 50.0f);
    EXPECT_EQ(absolutePtr->getBounds().x, 85.0f);
    EXPECT_EQ(absolutePtr->getBounds().y, 26.0f);
}

TEST(LclUiTest, MeasuredWidgetUsesLclConstraintsAndCanInvalidateMeasurement) {
    class Probe final : public MeasuredWidget {
    public:
        void setIntrinsicSize(layout::Size value) {
            intrinsicSize = value;
            invalidateMeasurement();
        }

        layout::Constraints lastConstraints{};
        layout::Size intrinsicSize{23.0f, 17.0f};
        int measureCount{0};

    protected:
        layout::Size measure(const layout::Constraints& constraints) override {
            lastConstraints = constraints;
            ++measureCount;
            return intrinsicSize;
        }
    };

    Probe probe;
    probe.calculateLayout();
    probe.syncLayout();
    EXPECT_EQ(probe.lastConstraints.width.mode, layout::MeasureMode::Undefined);
    EXPECT_EQ(probe.lastConstraints.height.mode, layout::MeasureMode::Undefined);
    EXPECT_EQ(probe.getBounds().width, 23.0f);
    EXPECT_EQ(probe.getBounds().height, 17.0f);
    const int initialMeasureCount = probe.measureCount;

    probe.setIntrinsicSize({41.0f, 29.0f});
    probe.calculateLayout();
    probe.syncLayout();
    EXPECT_GT(probe.measureCount, initialMeasureCount);
    EXPECT_EQ(probe.getBounds().width, 41.0f);
    EXPECT_EQ(probe.getBounds().height, 29.0f);

    auto root = std::make_unique<Container>();
    root->setWidth(100.0f);
    root->setHeight(60.0f);
    root->setAlignItems(layout::Align::FlexStart);
    auto constrained = std::make_unique<Probe>();
    Probe* constrainedPtr = constrained.get();
    root->addChild(std::move(constrained));
    root->calculateLayout(100.0f, 60.0f);
    root->syncLayout();
    EXPECT_EQ(constrainedPtr->lastConstraints.width.mode,
              layout::MeasureMode::AtMost);
    EXPECT_FLOAT_EQ(constrainedPtr->lastConstraints.width.value, 100.0f);
}

TEST(LclUiTest, WidgetTreeHierarchy) {
    RenderPass pass;
    auto root = std::make_unique<Container>();
    root->setRenderPass(&pass);
    root->setWidth(300.0f);
    root->setHeight(300.0f);

    auto btn = std::make_unique<Button>("Click Me");
    Button* btnPtr = btn.get();
    root->addChild(std::move(btn));

    root->calculateLayout(300.0f, 300.0f);
    root->syncLayout(0.0f, 0.0f);

    EXPECT_EQ(root->getBounds().width, 300.0f);
    EXPECT_EQ(btnPtr->getLabel(), "Click Me");
}

TEST(LclUiTest, RenderPassPropagatesToExistingDescendants) {
    RenderPass pass;
    auto root = std::make_unique<Container>();
    auto child = std::make_unique<Container>();
    Container* childPtr = child.get();
    child->setWidth(20.0f);
    child->setHeight(20.0f);
    root->addChild(std::move(child));
    root->setWidth(100.0f);
    root->setHeight(100.0f);
    root->calculateLayout(100.0f, 100.0f);
    root->syncLayout();
    root->setRenderPass(&pass);
    pass.clear();

    childPtr->invalidatePaint();
    EXPECT_TRUE(pass.hasDamage());
}

TEST(LclUiTest, ButtonStateAndClick) {
    auto btn = std::make_unique<Button>("Submit");
    btn->setWidth(100.0f);
    btn->setHeight(40.0f);
    btn->calculateLayout(100.0f, 40.0f);
    btn->syncLayout(0.0f, 0.0f);

    bool clicked = false;
    btn->setOnClick([&clicked]() {
        clicked = true;
    });

    EXPECT_EQ(btn->getState(), ButtonState::Normal);

    // Hover
    PointerEvent moveEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Enter};
    btn->onPointerEnter(moveEv);
    EXPECT_EQ(btn->getState(), ButtonState::Hover);

    // Mouse Down
    PointerEvent downEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Down};
    btn->onPointerDown(downEv);
    EXPECT_EQ(btn->getState(), ButtonState::Active);

    // Mouse Up -> triggers click
    PointerEvent upEv{10.0f, 10.0f, 0, 0.0f, 0.0f, PointerEventType::Up};
    btn->onPointerUp(upEv);
    EXPECT_TRUE(clicked);
    EXPECT_EQ(btn->getState(), ButtonState::Hover);
}

TEST(LclUiTest, RendererMapsLogicalCoordinatesToFractionalBufferPixels) {
    std::vector<uint32_t> pixels(6 * 6, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(6, 6, nullptr, pixels.data()));

    renderer.setDeviceScale(1.5f);
    renderer.drawRect({1.0f, 1.0f, 2.0f, 2.0f}, {255, 0, 0, 255});

    // Logical [1, 3) maps to physical [1.5, 4.5), i.e. the 3x3 raster area
    // bounded by integer pixels [1, 4). This is the same transform used by
    // WindowApp's 1.5x shared-memory surface.
    EXPECT_EQ(pixels[1 + 1 * 6], 0xFFFF0000u);
    EXPECT_EQ(pixels[3 + 3 * 6], 0xFFFF0000u);
    EXPECT_EQ(pixels[4 + 4 * 6], 0x00000000u);
}

TEST(LclUiTest, RendererMapsLogicalSubtreeToPhysicalOrigin) {
    std::vector<uint32_t> pixels(12 * 12, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(12, 12, nullptr, pixels.data()));

    renderer.setDeviceScale(1.5f);
    renderer.setContentOrigin(3.0f, 2.0f);
    renderer.drawRect({0.0f, 0.0f, 2.0f, 2.0f}, {0, 255, 0, 255});

    // The 2x2 logical rect becomes 3x3 physical pixels at its physical origin.
    EXPECT_EQ(pixels[3 + 2 * 12], 0xFF00FF00u);
    EXPECT_EQ(pixels[5 + 4 * 12], 0xFF00FF00u);
    EXPECT_EQ(pixels[6 + 5 * 12], 0x00000000u);
}

TEST(LclUiTest, TitlebarRadiusMatchesWindowMaskByDefault) {
    lcl::ui::chrome::WindowChromeStyle style;
    style.titleBarBackground = {17, 19, 23, 255};
    const auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        400.0f, 32.0f, 20.0f, "Window", 15.0f, style);
    const auto displayList = titleBar->buildChromeDisplayList(
        {0.0f, 0.0f, 400.0f, 32.0f});
    ASSERT_FALSE(displayList.commands().empty());
    const auto* background = std::get_if<graphics::DrawPathCommand>(
        &displayList.commands().front());
    ASSERT_NE(background, nullptr);
    ASSERT_NE(background->path.primitive(), nullptr);
    EXPECT_EQ(background->path.primitive()->kind,
              graphics::PathPrimitiveKind::TopRRect);
    EXPECT_FLOAT_EQ(background->path.primitive()->radiusX, 20.0f);
    EXPECT_FLOAT_EQ(background->path.primitive()->roundness,
                    style.titleBarRoundness);
    EXPECT_TRUE(titleBar->getChildren().empty());
}

TEST(LclUiTest, MountedTitlebarResizesWithoutReplacingItsWidgetTree) {
    auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        400.0f, 32.0f, 20.0f, "Window", 15.0f);
    auto* identity = titleBar.get();
    const float initialTitleWidth = titleBar->chromeLayout().titleWidth;
    titleBar->setFrameSize(640.0f, 32.0f);
    titleBar->calculateLayout(640.0f, 32.0f);
    titleBar->syncLayout();

    EXPECT_EQ(titleBar.get(), identity);
    EXPECT_FLOAT_EQ(titleBar->getAbsoluteBounds().width, 640.0f);
    EXPECT_GT(titleBar->chromeLayout().titleWidth, initialTitleWidth);
}

TEST(LclUiTest, EdgeToEdgeTitlebarDoesNotAddAnOpaqueBackground) {
    lcl::ui::chrome::WindowChromeStyle style;
    style.titleBarBackground = {0, 0, 0, 0};
    const auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        400.0f, 32.0f, 20.0f, "Window", 15.0f, style);
    const auto displayList = titleBar->buildChromeDisplayList(
        {0.0f, 0.0f, 400.0f, 32.0f});
    ASSERT_FALSE(displayList.commands().empty());
    const bool hasTitlebarBackground = std::any_of(
        displayList.commands().begin(), displayList.commands().end(),
        [](const auto& command) {
            const auto* path = std::get_if<graphics::DrawPathCommand>(&command);
            return path && path->path.primitive() &&
                path->path.primitive()->kind ==
                    graphics::PathPrimitiveKind::TopRRect;
        });
    EXPECT_FALSE(hasTitlebarBackground);
}

TEST(LclUiTest, TitlebarLayoutComesFromSharedCsdAndSsdChromeCore) {
    const lcl::ui::chrome::WindowChromeStyle style;
    const auto layout = lcl::ui::chrome::calculateWindowTitlebarLayout(
        540.0f, 34.0f, 20.0f, 14.0f, style);

    // CSD paint and SSD paint consume the exact same logical display list.
    EXPECT_FLOAT_EQ(layout.controlLeft, 12.0f);
    EXPECT_FLOAT_EQ(layout.controlTop, 12.0f);

    auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        540.0f, 34.0f, 20.0f, "LCL Terminal", 14.0f, style);
    const lcl::chrome::WindowChromeWidget ssdChrome("LCL Terminal", style);
    const auto ssdLayout = ssdChrome.layout(
        540.0f, 34.0f, 20.0f, 14.0f);
    EXPECT_FLOAT_EQ(ssdLayout.controlLeft, layout.controlLeft);
    EXPECT_FLOAT_EQ(ssdLayout.controlTop, layout.controlTop);
    EXPECT_FLOAT_EQ(ssdLayout.titleLeft, layout.titleLeft);
    EXPECT_FLOAT_EQ(ssdLayout.titleWidth, layout.titleWidth);
    EXPECT_EQ(ssdChrome.hitTest(layout.controlLeft + 1.0f,
                                layout.controlTop + 1.0f,
                                540.0f, 34.0f, 20.0f), 0);
    titleBar->calculateLayout(540.0f, 34.0f);
    titleBar->syncLayout();

    const graphics::RectF bounds{0.0f, 0.0f, 540.0f, 34.0f};
    const auto csdList = titleBar->buildChromeDisplayList(bounds);
    const auto ssdList = ssdChrome.buildDisplayList({
        bounds, 34.0f, 20.0f, 14.0f, 1.0f, true,
        style.titleBarBackground,
    });
    ASSERT_EQ(csdList.commands().size(), ssdList.commands().size());
    for (size_t index = 0; index < csdList.commands().size(); ++index) {
        const auto& csd = csdList.commands()[index];
        const auto& ssd = ssdList.commands()[index];
        ASSERT_EQ(csd.index(), ssd.index()) << "command " << index;
        if (const auto* csdConcat = std::get_if<graphics::ConcatCommand>(&csd)) {
            const auto& ssdTransform = std::get<graphics::ConcatCommand>(ssd).transform;
            EXPECT_FLOAT_EQ(csdConcat->transform.a, ssdTransform.a);
            EXPECT_FLOAT_EQ(csdConcat->transform.d, ssdTransform.d);
            EXPECT_FLOAT_EQ(csdConcat->transform.tx, ssdTransform.tx);
            EXPECT_FLOAT_EQ(csdConcat->transform.ty, ssdTransform.ty);
        } else if (const auto* csdPath = std::get_if<graphics::DrawPathCommand>(&csd)) {
            const auto& ssdPath = std::get<graphics::DrawPathCommand>(ssd);
            ASSERT_NE(csdPath->path.primitive(), nullptr);
            ASSERT_NE(ssdPath.path.primitive(), nullptr);
            EXPECT_EQ(csdPath->path.primitive()->kind,
                      ssdPath.path.primitive()->kind);
            EXPECT_EQ(csdPath->paint.color.toARGB(), ssdPath.paint.color.toARGB());
            EXPECT_EQ(csdPath->paint.style, ssdPath.paint.style);
            EXPECT_FLOAT_EQ(csdPath->paint.stroke.width,
                            ssdPath.paint.stroke.width);
        } else if (const auto* csdText = std::get_if<graphics::DrawTextCommand>(&csd)) {
            const auto& ssdText = std::get<graphics::DrawTextCommand>(ssd);
            EXPECT_EQ(csdText->text, ssdText.text);
            EXPECT_FLOAT_EQ(csdText->origin.x, ssdText.origin.x);
            EXPECT_FLOAT_EQ(csdText->origin.y, ssdText.origin.y);
            EXPECT_FLOAT_EQ(csdText->fontSize, ssdText.fontSize);
            EXPECT_EQ(csdText->color.toARGB(), ssdText.color.toARGB());
        }
    }
    EXPECT_TRUE(titleBar->getChildren().empty());
}

TEST(LclUiTest, WindowControlsAreGlyphFreeAndAnimateHoverPress) {
    auto canvas = std::make_unique<RecordingCanvas>();
    auto* canvasPtr = canvas.get();
    WindowApp app(std::move(canvas), 240, 40, "Window controls");
    const lcl::ui::chrome::WindowChromeStyle style;
    const auto layout = lcl::ui::chrome::calculateWindowTitlebarLayout(
        240.0f, 40.0f, 20.0f, 14.0f, style);
    auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
        240.0f, 40.0f, 20.0f, "Window", 14.0f, style);
    auto* titleBarPtr = titleBar.get();
    EXPECT_TRUE(titleBar->getChildren().empty());

    app.setRootWidget(std::move(titleBar));
    ASSERT_TRUE(app.renderFrame());
    EXPECT_EQ(canvasPtr->pathDrawCount, 6);
    ASSERT_FALSE(canvasPtr->texts.empty());
    EXPECT_EQ(canvasPtr->texts.back(), "Window");

    const float controlX = layout.controlLeft + style.controlSize * 0.5f;
    const float controlY = layout.controlTop + style.controlSize * 0.5f;
    app.sendPointerMove(controlX, controlY);
    for (int index = 0; index < 60; ++index) app.advanceAnimations(1.0f / 240.0f);
    const auto hoverVisual = titleBarPtr->sharedChrome().visual(0);
    EXPECT_GT(hoverVisual.scale, 1.0f);
    const auto hoverList = titleBarPtr->buildChromeDisplayList(
        {0.0f, 0.0f, 240.0f, 40.0f});
    ASSERT_GE(hoverList.commands().size(), 6u);
    const auto& hoverScale = std::get<graphics::ConcatCommand>(
        hoverList.commands()[2]).transform;
    const auto& hoverFill = std::get<graphics::DrawPathCommand>(
        hoverList.commands()[4]).paint;
    const auto& hoverStroke = std::get<graphics::DrawPathCommand>(
        hoverList.commands()[5]).paint;
    EXPECT_FLOAT_EQ(hoverScale.a, hoverVisual.scale);
    EXPECT_FLOAT_EQ(hoverScale.d, hoverVisual.scale);
    EXPECT_EQ(hoverFill.color.toARGB(), hoverVisual.background.toARGB());
    EXPECT_EQ(hoverStroke.color.toARGB(), hoverVisual.border.toARGB());

    EXPECT_TRUE(app.sendPointerDown(controlX, controlY));
    for (int index = 0; index < 60; ++index) app.advanceAnimations(1.0f / 240.0f);
    const auto pressedVisual = titleBarPtr->sharedChrome().visual(0);
    EXPECT_LT(pressedVisual.scale, 1.0f);
    const auto pressedList = titleBarPtr->buildChromeDisplayList(
        {0.0f, 0.0f, 240.0f, 40.0f});
    const auto& pressedFill = std::get<graphics::DrawPathCommand>(
        pressedList.commands()[4]).paint;
    EXPECT_EQ(pressedFill.color.toARGB(), pressedVisual.background.toARGB());

    EXPECT_TRUE(app.sendPointerUp(controlX, controlY));
    for (int index = 0; index < 90; ++index) app.advanceAnimations(1.0f / 240.0f);
    EXPECT_GT(titleBarPtr->sharedChrome().visual(0).scale, 1.0f);
}

TEST(LclUiTest, TopRoundedRectDoesNotLeakBelowItsCornerArc) {
    std::vector<uint32_t> pixels(64 * 64, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(64, 64, nullptr, pixels.data()));

    // 20px radius in a 32px-tall titlebar must keep its full outer radius.
    // A normal rounded rect clamps to 16px, while a patched flat-bottom strip
    // paints the pixel that should remain outside this top corner curve.
    renderer.drawTopRoundedRect({0.0f, 0.0f, 50.0f, 32.0f}, 20.0f,
                                {10, 20, 30, 255});

    EXPECT_EQ(pixels[0 + 13 * 64], 0x00000000u);
    EXPECT_EQ(pixels[0 + 21 * 64], 0xFF0A141Eu);
}

TEST(LclUiTest, RoundedRectPreservesTranslucentAlphaOnTransparentCanvas) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    renderer.drawRoundedRect({0.0f, 0.0f, 32.0f, 32.0f}, 8.0f,
                             {17, 19, 23, 184}, {}, 0.0f);

    EXPECT_EQ(pixels[16 + 16 * 32], 0xB8111317u);
}

TEST(LclUiTest, CpuStraightAlphaEntersGpuPremultipliedExactlyOnce) {
    constexpr uint32_t straight = 0x80ECEFF4u;
    constexpr uint32_t premultiplied = lcl::render::alpha::premultiplyArgb(straight);

    EXPECT_EQ(premultiplied, 0x8076787Au);
    EXPECT_EQ((premultiplied >> 24u) & 0xFFu, 128u);
    EXPECT_LE((premultiplied >> 16u) & 0xFFu, 128u);
    EXPECT_LE((premultiplied >> 8u) & 0xFFu, 128u);
    EXPECT_LE(premultiplied & 0xFFu, 128u);

    // A premultiplied intermediate copy is an identity operation. Applying
    // the CPU ingress conversion again demonstrates the old alpha-squaring bug.
    constexpr uint32_t copiedIntermediate = premultiplied;
    EXPECT_EQ(copiedIntermediate, premultiplied);
    EXPECT_NE(lcl::render::alpha::premultiplyArgb(premultiplied), premultiplied);
}

TEST(LclUiTest, GpuReadbackRestoresTheStraightAlphaShmContract) {
    constexpr uint32_t straight = 0x80ECEFF4u;
    constexpr uint32_t roundTrip = lcl::render::alpha::unpremultiplyArgb(
        lcl::render::alpha::premultiplyArgb(straight));

    const auto difference = [](uint32_t lhs, uint32_t rhs, uint32_t shift) {
        return std::abs(static_cast<int>((lhs >> shift) & 0xFFu) -
                        static_cast<int>((rhs >> shift) & 0xFFu));
    };
    EXPECT_EQ((roundTrip >> 24u) & 0xFFu, 128u);
    EXPECT_LE(difference(roundTrip, straight, 16u), 1);
    EXPECT_LE(difference(roundTrip, straight, 8u), 1);
    EXPECT_LE(difference(roundTrip, straight, 0u), 1);
    EXPECT_EQ(lcl::render::alpha::unpremultiplyArgb(0x00112233u), 0u);
    EXPECT_EQ(lcl::render::alpha::premultiplyArgb(0xFF123456u), 0xFF123456u);
}

TEST(LclUiTest, StraightAlphaBufferCompositesSourceAlphaAtFullGlobalOpacity) {
    std::vector<uint32_t> pixels(1, 0xFF0000FFu);
    const uint32_t source = 0x66FF0000u;
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(1, 1, nullptr, pixels.data()));

    renderer.drawBuffer(0, 0, 1, 1, &source, 1, 1.0f);

    EXPECT_EQ(pixels[0], 0xFF660099u);
}

TEST(LclUiTest, MaskedAndUnmaskedStraightAlphaBuffersMatchAtTheirCenters) {
    const uint32_t source = 0x6690C0F0u;
    const auto centerPixel = [&](float radius) {
        std::vector<uint32_t> pixels(5 * 5, 0xFF102030u);
        lcl::render::RasterRenderer renderer;
        EXPECT_TRUE(renderer.initialize(5, 5, nullptr, pixels.data()));
        renderer.drawBuffer(0, 0, 1, 1, &source, 1, 1.0f, radius, 2.0f,
                            false, 5, 5);
        return pixels[2 + 2 * 5];
    };

    EXPECT_EQ(centerPixel(2.0f), centerPixel(0.0f));
}

TEST(LclUiTest, TopLeftAnchoredPortraitBufferExtendsItsFinalPixelRow) {
    const uint32_t source[] = {
        0xFFFF0000u, 0xFF00FF00u,
        0xFF0000FFu, 0xFFFFFF00u,
    };
    std::vector<uint32_t> pixels(2 * 4, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(2, 4, nullptr, pixels.data()));

    renderer.drawBufferTransformed(
        0.0f, 0.0f, 2, 2, source, 2, 1.0f, 0.0f, 3.2f, false,
        2.0f, 4.0f,
        lcl::render::RasterBufferSampling::
            TopLeftAnchoredExtendTrailingEdge);

    EXPECT_EQ(pixels[0], 0xFFFF0000u);
    EXPECT_EQ(pixels[1], 0xFF00FF00u);
    for (int y = 1; y < 4; ++y) {
        EXPECT_EQ(pixels[static_cast<size_t>(y) * 2], 0xFF0000FFu);
        EXPECT_EQ(pixels[static_cast<size_t>(y) * 2 + 1], 0xFFFFFF00u);
    }
}

TEST(LclUiTest, TopLeftAnchoredLandscapeBufferExtendsItsFinalPixelColumn) {
    const uint32_t source[] = {
        0xFFFF0000u, 0xFF00FF00u,
        0xFF0000FFu, 0xFFFFFF00u,
    };
    std::vector<uint32_t> pixels(4 * 2, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(4, 2, nullptr, pixels.data()));

    renderer.drawBufferTransformed(
        0.0f, 0.0f, 2, 2, source, 2, 1.0f, 0.0f, 3.2f, false,
        4.0f, 2.0f,
        lcl::render::RasterBufferSampling::
            TopLeftAnchoredExtendTrailingEdge);

    EXPECT_EQ(pixels[0], 0xFFFF0000u);
    EXPECT_EQ(pixels[4], 0xFF0000FFu);
    for (int x = 1; x < 4; ++x) {
        EXPECT_EQ(pixels[x], 0xFF00FF00u);
        EXPECT_EQ(pixels[4 + x], 0xFFFFFF00u);
    }
}

TEST(LclUiTest, TopLeftAnchoredPortraitThumbnailCropsItsBottomEdge) {
    const uint32_t source[] = {
        0xFFFF0000u, 0xFF00FF00u,
        0xFF0000FFu, 0xFFFFFF00u,
        0xFF00FFFFu, 0xFFFF00FFu,
        0xFF101010u, 0xFF202020u,
    };
    std::vector<uint32_t> pixels(2 * 2, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(2, 2, nullptr, pixels.data()));

    renderer.drawBufferTransformed(
        0.0f, 0.0f, 2, 4, source, 2, 1.0f, 0.0f, 3.2f, false,
        2.0f, 2.0f,
        lcl::render::RasterBufferSampling::TopLeftAnchoredCropTrailingEdge);

    EXPECT_EQ(pixels[0], 0xFFFF0000u);
    EXPECT_EQ(pixels[1], 0xFF00FF00u);
    EXPECT_EQ(pixels[2], 0xFF0000FFu);
    EXPECT_EQ(pixels[3], 0xFFFFFF00u);
}

TEST(LclUiTest, TopLeftAnchoredLandscapeThumbnailCropsItsRightEdge) {
    const uint32_t source[] = {
        0xFFFF0000u, 0xFF00FF00u, 0xFF00FFFFu, 0xFF101010u,
        0xFF0000FFu, 0xFFFFFF00u, 0xFFFF00FFu, 0xFF202020u,
    };
    std::vector<uint32_t> pixels(2 * 2, 0u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(2, 2, nullptr, pixels.data()));

    renderer.drawBufferTransformed(
        0.0f, 0.0f, 4, 2, source, 4, 1.0f, 0.0f, 3.2f, false,
        2.0f, 2.0f,
        lcl::render::RasterBufferSampling::TopLeftAnchoredCropTrailingEdge);

    EXPECT_EQ(pixels[0], 0xFFFF0000u);
    EXPECT_EQ(pixels[1], 0xFF00FF00u);
    EXPECT_EQ(pixels[2], 0xFF0000FFu);
    EXPECT_EQ(pixels[3], 0xFFFFFF00u);
}

TEST(LclUiTest, StraightAlphaLayersAccumulateAlphaWithoutSquaringIt) {
    std::vector<uint32_t> pixels(1, 0x00000000u);
    const uint32_t red = 0x80FF0000u;
    const uint32_t green = 0x8000FF00u;
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(1, 1, nullptr, pixels.data()));

    renderer.drawBuffer(0, 0, 1, 1, &red, 1, 1.0f);
    renderer.drawBuffer(0, 0, 1, 1, &green, 1, 1.0f);

    EXPECT_EQ((pixels[0] >> 24) & 0xFFu, 192u);
    EXPECT_EQ((pixels[0] >> 16) & 0xFFu, 85u);
    EXPECT_EQ((pixels[0] >> 8) & 0xFFu, 170u);
    EXPECT_EQ(pixels[0] & 0xFFu, 0u);
}

TEST(LclUiTest, BackdropBlurCaptureClampsToSurfaceWithoutPreEntryBleed) {
    const int radius = lcl::render::gaussianKernelRadius(15.0f);
    EXPECT_EQ(radius, 23);

    const auto centered = lcl::render::computeBackdropFilterGeometry(
        100, 50, 200, 32, 1920, 1080, radius);
    EXPECT_EQ(centered.effect.x, 100);
    EXPECT_EQ(centered.effect.y, 50);
    EXPECT_EQ(centered.effect.width, 200);
    EXPECT_EQ(centered.effect.height, 32);
    EXPECT_EQ(centered.capture.x, 100);
    EXPECT_EQ(centered.capture.y, 50);
    EXPECT_EQ(centered.capture.width, 200);
    EXPECT_EQ(centered.capture.height, 32);
    EXPECT_EQ(centered.outputOffsetX, 0);
    EXPECT_EQ(centered.outputOffsetY, 0);
    EXPECT_EQ(centered.maskWidth, 200);
    EXPECT_EQ(centered.maskHeight, 32);
    EXPECT_EQ(centered.maskOffsetX, 0);
    EXPECT_EQ(centered.maskOffsetY, 0);
    EXPECT_FALSE(centered.clippedLeft);
    EXPECT_FALSE(centered.clippedRight);
    EXPECT_FALSE(centered.clippedTop);
    EXPECT_FALSE(centered.clippedBottom);

    const auto screenEdge = lcl::render::computeBackdropFilterGeometry(
        0, 0, 1920, 32, 1920, 1080, radius);
    EXPECT_EQ(screenEdge.effect.x, 0);
    EXPECT_EQ(screenEdge.effect.y, 0);
    EXPECT_EQ(screenEdge.effect.width, 1920);
    EXPECT_EQ(screenEdge.effect.height, 32);
    EXPECT_EQ(screenEdge.capture.x, 0);
    EXPECT_EQ(screenEdge.capture.y, 0);
    EXPECT_EQ(screenEdge.capture.width, 1920);
    EXPECT_EQ(screenEdge.capture.height, 32);
    EXPECT_EQ(screenEdge.outputOffsetX, 0);
    EXPECT_EQ(screenEdge.outputOffsetY, 0);
    EXPECT_FALSE(screenEdge.clippedLeft);
    EXPECT_FALSE(screenEdge.clippedRight);
    EXPECT_FALSE(screenEdge.clippedTop);
    EXPECT_FALSE(screenEdge.clippedBottom);

    const auto clippedRight = lcl::render::computeBackdropFilterGeometry(
        10, 0, 60, 40, 40, 40, 0);
    EXPECT_EQ(clippedRight.effect.x, 10);
    EXPECT_EQ(clippedRight.effect.width, 30);
    EXPECT_EQ(clippedRight.maskWidth, 60);
    EXPECT_EQ(clippedRight.maskHeight, 40);
    EXPECT_EQ(clippedRight.maskOffsetX, 0);
    EXPECT_EQ(clippedRight.maskOffsetY, 0);
    EXPECT_FALSE(clippedRight.clippedLeft);
    EXPECT_TRUE(clippedRight.clippedRight);
    EXPECT_FALSE(clippedRight.clippedTop);
    EXPECT_FALSE(clippedRight.clippedBottom);

    const auto clippedLeft = lcl::render::computeBackdropFilterGeometry(
        -20, 0, 60, 40, 40, 40, 0);
    EXPECT_EQ(clippedLeft.effect.x, 0);
    EXPECT_EQ(clippedLeft.effect.width, 40);
    EXPECT_EQ(clippedLeft.maskWidth, 60);
    EXPECT_EQ(clippedLeft.maskOffsetX, 20);
    EXPECT_TRUE(clippedLeft.clippedLeft);
    EXPECT_FALSE(clippedLeft.clippedRight);

    const auto clippedVertically = lcl::render::computeBackdropFilterGeometry(
        0, -10, 40, 60, 40, 40, 0);
    EXPECT_TRUE(clippedVertically.clippedTop);
    EXPECT_TRUE(clippedVertically.clippedBottom);
}

TEST(LclUiTest, BackdropMaskDoesNotCreateACornerAtFramebufferClipEdge) {
    const auto render = [](float effectX) {
        std::vector<uint32_t> pixels(40 * 40, 0xFF102030u);
        lcl::render::RasterRenderer renderer;
        EXPECT_TRUE(renderer.initialize(40, 40, nullptr, pixels.data()));
        renderer.applyBackdropFilter(
            effectX, 0, 60, 40, 10.0f, 2.0f, 1.0f,
            {{lcl::protocol::FilterType::Brightness, 1.0f}});
        return pixels;
    };

    const auto clippedRight = render(10.0f);
    EXPECT_EQ((clippedRight[0 * 40 + 39] >> 24) & 0xFFu, 255u);
    EXPECT_EQ((clippedRight[0 * 40 + 10] >> 24) & 0xFFu, 0u);

    const auto clippedLeft = render(-20.0f);
    EXPECT_EQ((clippedLeft[0 * 40 + 0] >> 24) & 0xFFu, 255u);
    EXPECT_EQ((clippedLeft[0 * 40 + 39] >> 24) & 0xFFu, 0u);
}

TEST(LclUiTest, SoftwareBackdropPathSkipsBlur) {
    std::vector<uint32_t> pixels{
        0xFF102030u, 0xFF405060u,
        0xFF708090u, 0xFFA0B0C0u,
    };
    const auto original = pixels;
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(2, 2, nullptr, pixels.data()));

    renderer.applyBackdropFilter(
        0, 0, 2, 2, 0.0f, 2.0f, 1.0f,
        {{lcl::protocol::FilterType::Blur, 16.0f}});

    EXPECT_EQ(pixels, original);
}

TEST(LclUiTest, SoftwareBackdropTintUsesTheSameFilteredMaterialPass) {
    std::vector<uint32_t> pixels{0xFF8090A0u};
    lcl::protocol::FilterOp tint{};
    tint.type = lcl::protocol::FilterType::Tint;
    tint.value = 0.5f;
    tint.params[0] = 16.0f;
    tint.params[1] = 32.0f;
    tint.params[2] = 48.0f;

    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(1, 1, nullptr, pixels.data()));
    renderer.applyBackdropFilter(0, 0, 1, 1, 0.0f, 2.0f, 1.0f, {tint});

    EXPECT_EQ(pixels[0], 0xFF485868u);
}

TEST(LclUiTest, SoftwareBackdropMaskUsesTheEffectRoundness) {
    const std::vector<lcl::protocol::FilterOp> filters{
        {lcl::protocol::FilterType::Brightness, 1.0f},
    };

    std::vector<uint32_t> circularPixels(16 * 16, 0xFF102030u);
    lcl::render::RasterRenderer circularRenderer;
    ASSERT_TRUE(circularRenderer.initialize(16, 16, nullptr, circularPixels.data()));
    circularRenderer.applyBackdropFilter(0, 0, 16, 16, 6.0f, 2.0f, 1.0f, filters);

    std::vector<uint32_t> superellipsePixels(16 * 16, 0xFF102030u);
    lcl::render::RasterRenderer superellipseRenderer;
    ASSERT_TRUE(superellipseRenderer.initialize(16, 16, nullptr, superellipsePixels.data()));
    superellipseRenderer.applyBackdropFilter(0, 0, 16, 16, 6.0f, 8.0f, 1.0f, filters);

    EXPECT_EQ(circularPixels[1 + 1 * 16] >> 24, 0u);
    EXPECT_EQ(superellipsePixels[1 + 1 * 16] >> 24, 0xFFu);
}

TEST(LclUiTest, RoundedRectPreservesSubpixelEdgeCoverageDuringScaleMotion) {
    const auto edgeAlphaAt = [](float x) {
        std::vector<uint32_t> pixels(16 * 16, 0x00000000u);
        lcl::render::RasterRenderer renderer;
        EXPECT_TRUE(renderer.initialize(16, 16, nullptr, pixels.data()));
        renderer.drawRoundedRect({x, 2.0f, 8.0f, 8.0f}, 2.0f,
                                 {255, 255, 255, 255}, {}, 0.0f);
        return static_cast<uint8_t>((pixels[0 + 6 * 16] >> 24) & 0xFFu);
    };

    const uint8_t quarterPixel = edgeAlphaAt(0.25f);
    const uint8_t halfPixel = edgeAlphaAt(0.50f);
    EXPECT_GT(quarterPixel, 0u);
    EXPECT_LT(quarterPixel, 255u);
    EXPECT_GT(halfPixel, 0u);
    EXPECT_LT(halfPixel, quarterPixel);
}

TEST(LclUiTest, ScrollViewClampsOffsetWhenContentLargerThanViewport) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(300.0f);
    scrollView->setContent(std::move(content));

    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EXPECT_FLOAT_EQ(scrollView->getContentHeight(), 300.0f);
    EXPECT_FLOAT_EQ(scrollView->getMaxScrollY(), 200.0f);

    scrollView->setScrollY(-50.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);

    scrollView->setScrollY(120.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 120.0f);

    scrollView->setScrollY(500.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 200.0f);
}

TEST(LclUiTest, ScrollViewSkipsDirtyWorkWhenClampedOffsetDoesNotChange) {
    RenderPass pass;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setRenderPass(&pass);
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(300.0f);
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();
    pass.clear();

    const uint64_t paintRevision = scrollView->getPaintRevision();
    const uint64_t presentationRevision =
        scrollView->getPresentationRevision();

    scrollView->setScrollY(-20.0f);
    EXPECT_FALSE(pass.hasDamage());

    scrollView->setScrollY(80.0f);
    EXPECT_TRUE(pass.hasDamage());
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 80.0f);
    EXPECT_EQ(scrollView->getPaintRevision(), paintRevision);
    EXPECT_GT(scrollView->getPresentationRevision(), presentationRevision);

    pass.clear();
    scrollView->setScrollY(80.0f);
    EXPECT_FALSE(pass.hasDamage());

    scrollView->setScrollY(500.0f);
    EXPECT_TRUE(pass.hasDamage());
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 200.0f);

    pass.clear();
    scrollView->setScrollY(600.0f);
    EXPECT_FALSE(pass.hasDamage());
}

TEST(LclUiTest, PureScrollChangesOnlyTheScrollViewPresentationRevision) {
    WindowApp app(std::make_unique<RecordingCanvas>(), 200, 100,
                  "Scroll revision channels");
    auto scrollView = std::make_unique<ScrollView>();
    ScrollView* scrollPtr = scrollView.get();
    auto content = std::make_unique<Container>();
    Container* contentPtr = content.get();
    content->setHeight(300.0f);
    scrollView->setContent(std::move(content));
    app.setRootWidget(std::move(scrollView));
    ASSERT_TRUE(app.renderFrame());

    const uint64_t scrollLayout = scrollPtr->getLayoutRevision();
    const uint64_t scrollPaint = scrollPtr->getPaintRevision();
    const uint64_t scrollPresentation = scrollPtr->getPresentationRevision();
    const uint64_t contentPaint = contentPtr->getPaintRevision();
    const uint64_t contentPresentation = contentPtr->getPresentationRevision();

    scrollPtr->setScrollY(40.0f);

    EXPECT_EQ(scrollPtr->getLayoutRevision(), scrollLayout);
    EXPECT_EQ(scrollPtr->getPaintRevision(), scrollPaint);
    EXPECT_GT(scrollPtr->getPresentationRevision(), scrollPresentation);
    EXPECT_EQ(contentPtr->getPaintRevision(), contentPaint);
    EXPECT_EQ(contentPtr->getPresentationRevision(), contentPresentation);
    EXPECT_FALSE(scrollPtr->isLayoutDirty());
}

TEST(LclUiTest, ScrollViewPatchesPaintAndRebuildsOnlyForGeometryChanges) {
    RecordingCanvas canvas;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    auto paintedChild = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* paintedChildPointer = paintedChild.get();
    paintedChild->setWidth(200.0f);
    paintedChild->setHeight(300.0f);
    content->addChild(std::move(paintedChild));
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();

    const graphics::RectF fullDamage{-1000.0f, -1000.0f, 4000.0f, 4000.0f};
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 1);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerDrawCount, 1);

    scrollView->setScrollY(40.0f);
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 1);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerDrawCount, 2);
    ASSERT_FALSE(canvas.cachedLayerDestinations.empty());
    EXPECT_FLOAT_EQ(canvas.cachedLayerDestinations.back().y, -40.0f);

    paintedChildPointer->invalidatePaint();
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 2);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerUpdateBeginCount, 1);

    paintedChildPointer->setHeight(340.0f);
    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 3);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 2);

    scrollView->setHeight(120.0f);
    scrollView->calculateLayout(200.0f, 120.0f);
    scrollView->syncLayout();
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 4);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 3);
    ASSERT_FALSE(canvas.clips.empty());
    EXPECT_FLOAT_EQ(canvas.clips.back().width, 200.0f);
    EXPECT_FLOAT_EQ(canvas.clips.back().height, 120.0f);

    // Cached-layer identity includes its logical source origin. Moving the
    // ScrollView in a parent layout must rebuild rather than partially patch a
    // newly allocated empty rasterd cache.
    scrollView->syncLayout(12.0f, 8.0f);
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(paintedChildPointer->paintCount, 5);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 4);
}

TEST(LclUiTest, ScrollViewPatchesEveryPaintRegionInOneRenderPass) {
    RecordingCanvas canvas;
    RenderPass pass;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(120.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(360.0f);

    auto first = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* firstPtr = first.get();
    first->setPositionType(layout::PositionType::Absolute);
    first->setPosition(layout::Edge::Top, 10.0f);
    first->setWidth(200.0f);
    first->setHeight(30.0f);
    content->addChild(std::move(first));

    auto second = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* secondPtr = second.get();
    second->setPositionType(layout::PositionType::Absolute);
    second->setPosition(layout::Edge::Top, 70.0f);
    second->setWidth(200.0f);
    second->setHeight(30.0f);
    content->addChild(std::move(second));

    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 120.0f);
    scrollView->syncLayout();
    scrollView->setRenderPass(&pass);
    pass.clear();

    const graphics::RectF fullDamage{-1000.0f, -1000.0f, 4000.0f, 4000.0f};
    scrollView->draw(canvas, fullDamage);
    ASSERT_EQ(canvas.cachedLayerBeginCount, 1);
    ASSERT_EQ(firstPtr->paintCount, 1);
    ASSERT_EQ(secondPtr->paintCount, 1);

    firstPtr->invalidatePaint();
    secondPtr->invalidatePaint();
    const std::vector<graphics::RectF> frameDamage = pass.getDirtyRects();
    ASSERT_EQ(frameDamage.size(), 2u);
    pass.clear();
    pass.begin(canvas, frameDamage);
    for (const graphics::RectF& damage : frameDamage) {
        scrollView->draw(canvas, damage);
    }
    pass.end(canvas);

    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerUpdateBeginCount, 2);
    EXPECT_EQ(firstPtr->paintCount, 2);
    EXPECT_EQ(secondPtr->paintCount, 2);
    ASSERT_EQ(canvas.cachedLayerUpdateBounds.size(), 2u);
    EXPECT_LT(canvas.cachedLayerUpdateBounds.front().height, 40.0f);
    EXPECT_LT(canvas.cachedLayerUpdateBounds.back().height, 40.0f);
}

TEST(LclUiTest, ScrollViewButtonAndSpinnerSharePartialCacheUpdates) {
    RecordingCanvas canvas;
    RenderPass pass;
    MotionCoordinator coordinator;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(120.0f);
    scrollView->setRenderPass(&pass);
    scrollView->setMotionCoordinator(&coordinator);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(320.0f);

    auto button = std::make_unique<Button>("Press");
    Button* buttonPtr = button.get();
    button->setPositionType(layout::PositionType::Absolute);
    button->setPosition(layout::Edge::Top, 10.0f);
    button->setWidth(140.0f);
    button->setHeight(40.0f);
    content->addChild(std::move(button));

    auto spinner = std::make_unique<ProgressView>();
    spinner->setPositionType(layout::PositionType::Absolute);
    spinner->setPosition(layout::Edge::Top, 75.0f);
    content->addChild(std::move(spinner));

    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 120.0f);
    scrollView->syncLayout();
    pass.clear();

    const graphics::RectF fullDamage{-1000.0f, -1000.0f, 4000.0f, 4000.0f};
    scrollView->draw(canvas, fullDamage);
    ASSERT_EQ(canvas.cachedLayerBeginCount, 1);
    ASSERT_TRUE(coordinator.hasActiveAnimations());

    pass.clear();
    ASSERT_TRUE(buttonPtr->onPointerDown(PointerEvent{}));
    coordinator.tick(1.0f / 60.0f);
    const std::vector<graphics::RectF> frameDamage = pass.getDirtyRects();
    ASSERT_FALSE(frameDamage.empty());
    pass.clear();
    pass.begin(canvas, frameDamage);
    for (const graphics::RectF& damage : frameDamage) {
        scrollView->draw(canvas, damage);
    }
    pass.end(canvas);

    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_GT(canvas.cachedLayerUpdateBeginCount, 0);
}

TEST(LclUiTest, ScrollAndSpinnerKeepCacheUpdateAtSpinnerBounds) {
    RecordingCanvas canvas;
    RenderPass pass;
    MotionCoordinator coordinator;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(120.0f);
    scrollView->setRenderPass(&pass);
    scrollView->setMotionCoordinator(&coordinator);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(320.0f);
    auto spinner = std::make_unique<ProgressView>();
    spinner->setPositionType(layout::PositionType::Absolute);
    spinner->setPosition(layout::Edge::Top, 75.0f);
    content->addChild(std::move(spinner));
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 120.0f);
    scrollView->syncLayout();
    pass.clear();

    const graphics::RectF fullDamage{-1000.0f, -1000.0f, 4000.0f, 4000.0f};
    scrollView->draw(canvas, fullDamage);
    ASSERT_EQ(canvas.cachedLayerBeginCount, 1);
    ASSERT_TRUE(coordinator.hasActiveAnimations());

    pass.clear();
    coordinator.tick(1.0f / 60.0f);
    ASSERT_FALSE(pass.getRasterDirtyRects().empty());
    scrollView->setScrollY(10.0f);
    const std::vector<graphics::RectF> frameDamage = pass.getDirtyRects();
    const std::vector<graphics::RectF> frameRasterDamage =
        pass.getRasterDirtyRects();
    ASSERT_EQ(frameDamage.size(), 1u);
    ASSERT_EQ(frameRasterDamage.size(), 1u);
    EXPECT_GE(frameDamage.front().height, 120.0f);
    EXPECT_LT(frameRasterDamage.front().height, 30.0f);

    pass.clear();
    pass.begin(canvas, frameDamage, frameRasterDamage);
    for (const graphics::RectF& damage : frameDamage) {
        scrollView->draw(canvas, damage);
    }
    pass.end(canvas);

    ASSERT_EQ(canvas.cachedLayerUpdateBeginCount, 1);
    ASSERT_EQ(canvas.cachedLayerUpdateBounds.size(), 1u);
    EXPECT_LT(canvas.cachedLayerUpdateBounds.front().width, 40.0f);
    EXPECT_LT(canvas.cachedLayerUpdateBounds.front().height, 40.0f);
}

TEST(LclUiTest, WindowAppSliderValueAndTextPatchScrollCache) {
    auto canvas = std::make_unique<RecordingCanvas>();
    RecordingCanvas* recorded = canvas.get();
    WindowApp app(std::move(canvas), 240, 160, "Slider cache damage");

    auto root = std::make_unique<Container>();
    root->setWidth(240.0f);
    root->setHeight(160.0f);
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(120.0f);
    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(360.0f);

    auto status = std::make_unique<Text>("Value: 35");
    Text* statusPtr = status.get();
    status->setPositionType(layout::PositionType::Absolute);
    status->setPosition(layout::Edge::Top, 10.0f);
    content->addChild(std::move(status));

    auto slider = std::make_unique<Slider>(35.0f, 0.0f, 100.0f, 1.0f);
    Slider* sliderPtr = slider.get();
    slider->setPositionType(layout::PositionType::Absolute);
    slider->setPosition(layout::Edge::Top, 50.0f);
    slider->setWidth(180.0f);
    slider->setOnChange([statusPtr](float value) {
        statusPtr->setText(
            "Value: " + std::to_string(static_cast<int>(value)));
    });
    content->addChild(std::move(slider));

    scrollView->setContent(std::move(content));
    root->addChild(std::move(scrollView));
    app.setRootWidget(std::move(root));

    ASSERT_TRUE(app.renderFrame());
    ASSERT_EQ(recorded->cachedLayerBeginCount, 1);
    sliderPtr->setValue(88.0f);
    ASSERT_TRUE(app.renderFrame());

    EXPECT_EQ(recorded->cachedLayerBeginCount, 1);
    EXPECT_GT(recorded->cachedLayerUpdateBeginCount, 0);
}

TEST(LclUiTest, ScrollViewDoesNotRerasterLongContentEachAnimationTick) {
    RecordingCanvas canvas;
    MotionCoordinator coordinator;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);
    scrollView->setMotionCoordinator(&coordinator);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    auto animatedChild = std::make_unique<CountingPaintWidget>();
    CountingPaintWidget* childPtr = animatedChild.get();
    animatedChild->setWidth(200.0f);
    animatedChild->setHeight(300.0f);
    content->addChild(std::move(animatedChild));
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();

    const graphics::RectF fullDamage{-1000.0f, -1000.0f, 4000.0f, 4000.0f};
    scrollView->draw(canvas, fullDamage);
    ASSERT_EQ(canvas.cachedLayerBeginCount, 1);
    const int initialPaints = childPtr->paintCount;

    coordinator.animateFloat(
        *childPtr, AnimatableProperty::ScaleX, 1.0f, 0.96f,
        Motion::tween(0.2f, Easing::linear()),
        [childPtr](float value) {
            childPtr->applyPresentationValue(AnimatableProperty::ScaleX, value);
        });
    coordinator.tick(0.05f);
    ASSERT_TRUE(childPtr->hasActiveAnimationInSubtree());
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerUpdateBeginCount, 1);
    EXPECT_GT(childPtr->paintCount, initialPaints);

    coordinator.tick(0.2f);
    ASSERT_FALSE(childPtr->hasActiveAnimationInSubtree());
    scrollView->draw(canvas, fullDamage);
    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerUpdateBeginCount, 2);
}

TEST(LclUiTest, ScrollViewDefersOffscreenProceduralPresentationRaster) {
    RecordingCanvas canvas;
    MotionCoordinator coordinator;
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);
    scrollView->setMotionCoordinator(&coordinator);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(400.0f);
    auto spinner = std::make_unique<ProgressView>();
    spinner->setPositionType(layout::PositionType::Absolute);
    spinner->setPosition(layout::Edge::Top, 300.0f);
    content->addChild(std::move(spinner));
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout();

    const graphics::RectF fullDamage{-1000.0f, -1000.0f, 4000.0f, 4000.0f};
    scrollView->draw(canvas, fullDamage);
    ASSERT_EQ(canvas.cachedLayerBeginCount, 1);
    ASSERT_EQ(canvas.cachedLayerBounds.size(), 1u);
    const graphics::RectF stableCacheSource = canvas.cachedLayerBounds.front();
    ASSERT_TRUE(coordinator.hasActiveAnimations());

    const uint64_t offscreenRevision =
        scrollView->getContent()->getPresentationRevision();
    coordinator.tick(1.0f / 60.0f);
    EXPECT_EQ(scrollView->getContent()->getPresentationRevision(),
              offscreenRevision);
    scrollView->draw(canvas, fullDamage);

    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerUpdateBeginCount, 0);
    EXPECT_EQ(canvas.cachedLayerDrawCount, 2);

    // Revealing the spinner must update the existing content cache in its
    // stable, unscrolled coordinate system. A scroll-shifted source origin
    // would make rasterd allocate an empty cache and preserve only the spinner
    // update, causing every other ScrollView element to disappear.
    scrollView->setScrollY(250.0f);
    scrollView->draw(canvas, fullDamage);

    EXPECT_EQ(canvas.cachedLayerBeginCount, 1);
    EXPECT_EQ(canvas.cachedLayerUpdateBeginCount, 1);
    ASSERT_EQ(canvas.cachedLayerBounds.size(), 2u);
    const graphics::RectF partialUpdateSource = canvas.cachedLayerBounds.back();
    EXPECT_FLOAT_EQ(partialUpdateSource.x, stableCacheSource.x);
    EXPECT_FLOAT_EQ(partialUpdateSource.y, stableCacheSource.y);
    EXPECT_FLOAT_EQ(partialUpdateSource.width, stableCacheSource.width);
    EXPECT_FLOAT_EQ(partialUpdateSource.height, stableCacheSource.height);
    ASSERT_EQ(canvas.cachedLayerUpdateBounds.size(), 1u);
    EXPECT_GT(canvas.cachedLayerUpdateBounds.front().y,
              stableCacheSource.y + 200.0f);
    ASSERT_EQ(canvas.cachedLayerDestinations.size(), 3u);
    EXPECT_FLOAT_EQ(canvas.cachedLayerDestinations.back().y,
                    stableCacheSource.y - 250.0f);
}

TEST(LclUiTest, ScrollViewCachedLayerRespectsViewportClip) {
    std::vector<uint32_t> pixels(64 * 64, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(64, 64, nullptr, pixels.data()));
    lcl::render::RasterCanvas canvas(renderer);

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(32.0f);
    scrollView->setHeight(16.0f);
    auto content = std::make_unique<Container>();
    content->setWidth(32.0f);
    auto paintedChild = std::make_unique<CountingPaintWidget>();
    paintedChild->setWidth(32.0f);
    paintedChild->setHeight(40.0f);
    content->addChild(std::move(paintedChild));
    scrollView->setContent(std::move(content));
    scrollView->calculateLayout(32.0f, 16.0f);
    scrollView->syncLayout();

    canvas.beginFrame();
    scrollView->draw(canvas, {-100.0f, -100.0f, 300.0f, 300.0f});
    canvas.endFrame();

    EXPECT_EQ(pixels[4 + 4 * 64], 0xFF14283Cu);
    EXPECT_EQ(pixels[4 + 20 * 64], 0xFF14161Du);
    const auto& commands = canvas.lastDisplayList().commands();
    EXPECT_EQ(std::count_if(commands.begin(), commands.end(), [](const auto& command) {
        return std::holds_alternative<graphics::BeginCachedLayerCommand>(command);
    }), 1);
    EXPECT_EQ(std::count_if(commands.begin(), commands.end(), [](const auto& command) {
        return std::holds_alternative<graphics::EndCachedLayerCommand>(command);
    }), 1);
    EXPECT_EQ(std::count_if(commands.begin(), commands.end(), [](const auto& command) {
        return std::holds_alternative<graphics::DrawCachedLayerCommand>(command);
    }), 1);
}

TEST(LclUiTest, ScrollViewHandlesWheelScroll) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);
    scrollView->setScrollSpeed(20.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(400.0f);
    scrollView->setContent(std::move(content));

    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);

    PointerEvent scrollDown{};
    scrollDown.type = PointerEventType::Scroll;
    scrollDown.deltaY = 2.0f;

    EXPECT_TRUE(scrollView->onScroll(scrollDown));
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 40.0f);

    PointerEvent scrollUp{};
    scrollUp.type = PointerEventType::Scroll;
    scrollUp.deltaY = -1.0f;

    EXPECT_TRUE(scrollView->onScroll(scrollUp));
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 20.0f);
}

TEST(LclUiTest, ScrollViewZeroOffsetWhenContentSmallerThanViewport) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(300.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(150.0f);
    scrollView->setContent(std::move(content));

    scrollView->calculateLayout(200.0f, 300.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EXPECT_FLOAT_EQ(scrollView->getContentHeight(), 150.0f);
    EXPECT_FLOAT_EQ(scrollView->getMaxScrollY(), 0.0f);

    scrollView->setScrollY(100.0f);
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);

    PointerEvent scrollEv{};
    scrollEv.type = PointerEventType::Scroll;
    scrollEv.deltaY = 5.0f;
    EXPECT_FALSE(scrollView->onScroll(scrollEv));
    EXPECT_FLOAT_EQ(scrollView->getScrollY(), 0.0f);
}

TEST(LclUiTest, ScrollViewHitTestRoutesToScrolledChild) {
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setWidth(200.0f);
    scrollView->setHeight(100.0f);

    auto content = std::make_unique<Container>();
    content->setWidth(200.0f);
    content->setHeight(500.0f);

    auto item1 = std::make_unique<Button>("Item 1");
    item1->setWidth(200.0f);
    item1->setHeight(50.0f);

    auto item2 = std::make_unique<Button>("Item 2");
    item2->setWidth(200.0f);
    item2->setHeight(50.0f);
    item2->setPositionType(layout::PositionType::Absolute);
    item2->setPosition(layout::Edge::Top, 200.0f);

    Button* item1Ptr = item1.get();
    Button* item2Ptr = item2.get();

    content->addChild(std::move(item1));
    content->addChild(std::move(item2));
    scrollView->setContent(std::move(content));

    scrollView->calculateLayout(200.0f, 100.0f);
    scrollView->syncLayout(0.0f, 0.0f);

    EventDispatcher dispatcher;

    // At scrollY = 0: Point (50, 25) hits item 1
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 25.0f), item1Ptr);

    // Point (50, 80) is inside ScrollView, but below item1 and above item2 -> hits content container
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 80.0f), scrollView->getContent());

    // Scroll by 200px so item 2 moves into viewport Y: [0, 50]
    scrollView->setScrollY(200.0f);

    // Point (50, 25) now hits item 2
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 25.0f), item2Ptr);

    // Point (50, 225) is physically outside ScrollView viewport (height 100) -> hitTest returns nullptr
    EXPECT_EQ(dispatcher.hitTest(scrollView.get(), 50.0f, 225.0f), nullptr);
}

TEST(LclUiTest, CanvasClipDiscardsPrimitivesCompletelyOutside) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    renderer.setRetainsFrameBacking(true);
    lcl::render::RasterCanvas canvas(renderer);

    canvas.beginFrame();
    canvas.clipRect(graphics::RectF{0.0f, 0.0f, 10.0f, 10.0f});
    canvas.drawRect(graphics::RectF{15.0f, 15.0f, 10.0f, 10.0f}, graphics::Color{255, 255, 255, 255});
    canvas.endFrame();

    for (uint32_t p : pixels) {
        EXPECT_EQ(p, 0x00000000u);
    }
}

TEST(LclUiTest, CanvasClipClipsPartiallyIntersectingRect) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    renderer.setRetainsFrameBacking(true);
    lcl::render::RasterCanvas canvas(renderer);

    canvas.beginFrame();
    canvas.clipRect(graphics::RectF{0.0f, 0.0f, 16.0f, 16.0f});
    canvas.drawRect(graphics::RectF{0.0f, 0.0f, 32.0f, 32.0f}, graphics::Color{255, 255, 255, 255});
    canvas.endFrame();

    // Inside clip
    EXPECT_EQ(pixels[5 + 5 * 32], 0xFFFFFFFFu);
    EXPECT_EQ(pixels[15 + 15 * 32], 0xFFFFFFFFu);

    // Outside clip
    EXPECT_EQ(pixels[17 + 5 * 32], 0x00000000u);
    EXPECT_EQ(pixels[5 + 17 * 32], 0x00000000u);
    EXPECT_EQ(pixels[20 + 20 * 32], 0x00000000u);
}

TEST(LclUiTest, CanvasClipClipsTextRendering) {
    std::vector<uint32_t> pixels(64 * 64, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(64, 64, nullptr, pixels.data()));
    renderer.setRetainsFrameBacking(true);
    lcl::render::RasterCanvas canvas(renderer);

    canvas.beginFrame();
    canvas.clipRect(graphics::RectF{0.0f, 0.0f, 32.0f, 20.0f});
    // Draw text outside clip
    canvas.drawText(0.0f, 40.0f, "Out of bounds text", graphics::Color{255, 255, 255, 255}, 14.0f, graphics::FontFamily::Interface);
    canvas.endFrame();

    for (int y = 30; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            EXPECT_EQ(pixels[x + y * 64], 0x00000000u);
        }
    }
}

TEST(LclUiTest, CanvasClipSaveAndRestoreRestoresPreviousClip) {
    std::vector<uint32_t> pixels(32 * 32, 0x00000000u);
    lcl::render::RasterRenderer renderer;
    ASSERT_TRUE(renderer.initialize(32, 32, nullptr, pixels.data()));
    renderer.setRetainsFrameBacking(true);
    lcl::render::RasterCanvas canvas(renderer);

    canvas.beginFrame();
    canvas.clipRect(graphics::RectF{0.0f, 0.0f, 24.0f, 24.0f});
    canvas.saveState();
    canvas.clipRect(graphics::RectF{0.0f, 0.0f, 8.0f, 8.0f});

    // Draw while inner clip is active
    canvas.drawRect(graphics::RectF{12.0f, 12.0f, 4.0f, 4.0f}, graphics::Color{255, 0, 0, 255});

    canvas.restoreState();

    // After restore, outer clip [0..24, 0..24] is active again
    canvas.drawRect(graphics::RectF{12.0f, 12.0f, 4.0f, 4.0f}, graphics::Color{0, 255, 0, 255});
    EXPECT_EQ(pixels[13 + 13 * 32], 0x00000000u); // Still record-only.
    canvas.endFrame();
    EXPECT_EQ(pixels[13 + 13 * 32], 0xFF00FF00u); // Drawn successfully
}
