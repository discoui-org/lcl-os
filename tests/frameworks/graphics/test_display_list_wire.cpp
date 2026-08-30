#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "lcl-graphics/display_list_wire.hpp"

namespace graphics = lcl::graphics;

TEST(DisplayListWireTest, RoundTripsBackendNeutralCommandsCanonically) {
    graphics::DisplayListBuilder builder;
    builder.save();
    builder.concat({1.25f, 0.0f, 0.0f, 0.75f, 12.0f, -3.0f});
    builder.beginLayer(0.8f);
    builder.clipRect({1.0f, 2.0f, 300.0f, 180.0f});

    graphics::Path clipPath;
    clipPath.moveTo(4.0f, 5.0f)
        .quadTo(9.0f, 2.0f, 14.0f, 5.0f)
        .arcTo(3.0f, 4.0f, 0.25f, false, true, 20.0f, 11.0f)
        .close();
    builder.clipPath(clipPath, graphics::FillRule::EvenOdd);
    builder.clearRect({0.0f, 0.0f, 20.0f, 10.0f}, {1, 2, 3, 4});
    builder.applyBackdropEffects(
        {4.0f, 5.0f, 80.0f, 32.0f}, 12.0f, 2.0f, 1.0f,
        {{graphics::EffectType::Blur, 8.0f}});
    builder.beginCachedLayerUpdate(42, {0.0f, 0.0f, 200.0f, 100.0f},
                                   {5.0f, 6.0f, 20.0f, 30.0f});
    builder.endCachedLayer();
    builder.drawCachedLayerTransformed(
        42, {10.0f, 20.0f, 200.0f, 100.0f}, 0.7f,
        {0.9f, 0.1f, -0.1f, 0.9f, 4.0f, 5.0f});

    graphics::Path rounded;
    rounded.addRRect({{8.0f, 9.0f, 100.0f, 40.0f}, 7.0f, 8.0f, 3.5f});
    graphics::Paint paint{};
    paint.color = {10, 20, 30, 200};
    paint.style = graphics::PaintStyle::Stroke;
    paint.fillRule = graphics::FillRule::EvenOdd;
    paint.stroke.width = 2.5f;
    paint.stroke.cap = graphics::StrokeCap::Round;
    paint.stroke.join = graphics::StrokeJoin::Bevel;
    paint.stroke.miterLimit = 6.0f;
    paint.stroke.scaling = graphics::StrokeScaling::Hairline;
    paint.opacity = 0.65f;
    builder.drawPath(rounded, paint);
    builder.drawText({13.0f, 17.0f}, "LCL UTF-8: çığ", {40, 50, 60, 255},
                     15.0f, graphics::FontFamily::Monospace);
    builder.endLayer();
    builder.restore();

    const auto encoded = graphics::encodeDisplayList(builder.build());
    ASSERT_TRUE(encoded);
    const auto decoded = graphics::decodeDisplayList(encoded.bytes);
    ASSERT_TRUE(decoded);
    const auto reencoded = graphics::encodeDisplayList(decoded.displayList);
    ASSERT_TRUE(reencoded);
    EXPECT_EQ(reencoded.bytes, encoded.bytes);

    const auto& commands = decoded.displayList.commands();
    ASSERT_GE(commands.size(), 2u);
    const auto* cachedDraw = std::get_if<graphics::DrawCachedLayerCommand>(
        &commands[commands.size() - 5]);
    ASSERT_NE(cachedDraw, nullptr);
    EXPECT_FLOAT_EQ(cachedDraw->transform.a, 0.9f);
    EXPECT_FLOAT_EQ(cachedDraw->transform.b, 0.1f);
    EXPECT_FLOAT_EQ(cachedDraw->transform.tx, 4.0f);
    EXPECT_FLOAT_EQ(cachedDraw->transform.ty, 5.0f);
    const auto* pathCommand = std::get_if<graphics::DrawPathCommand>(
        &commands[commands.size() - 4]);
    ASSERT_NE(pathCommand, nullptr);
    ASSERT_NE(pathCommand->path.primitive(), nullptr);
    EXPECT_EQ(pathCommand->path.primitive()->kind,
              graphics::PathPrimitiveKind::RRect);
}

TEST(DisplayListWireTest, RoundTripsStableImageResourceReferences) {
    graphics::DisplayListBuilder builder;
    builder.drawImage({0.0f, 0.0f, 32.0f, 32.0f}, 0x1234u,
                      32, 32, 32, 0.75f, 4.0f, 2.0f, false,
                      41, 7, true);

    const auto encoded = graphics::encodeDisplayList(builder.build());
    ASSERT_TRUE(encoded);
    const auto decoded = graphics::decodeDisplayList(encoded.bytes);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.displayList.commands().size(), 1u);
    const auto* image = std::get_if<graphics::DrawImageCommand>(
        &decoded.displayList.commands().front());
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->resourceKey, 0u);
    EXPECT_EQ(image->resourceId, 41u);
    EXPECT_EQ(image->contentRevision, 7u);
    EXPECT_TRUE(image->opaque);
    EXPECT_FLOAT_EQ(image->opacity, 0.75f);
}

TEST(DisplayListWireTest, RoundTripsExternalBufferPlaceholderWithoutDescriptor) {
    graphics::DisplayListBuilder builder;
    builder.drawExternalBuffer(73, {4.0f, 5.0f, 160.0f, 90.0f});

    const auto encoded = graphics::encodeDisplayList(builder.build());
    ASSERT_TRUE(encoded);
    const auto decoded = graphics::decodeDisplayList(encoded.bytes);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.displayList.commands().size(), 1u);
    const auto* external =
        std::get_if<graphics::DrawExternalBufferCommand>(
            &decoded.displayList.commands().front());
    ASSERT_NE(external, nullptr);
    EXPECT_EQ(external->nodeId, 73u);
    EXPECT_EQ(external->bufferId, 0u);
    EXPECT_EQ(external->resourceKey, 0u);
    EXPECT_EQ(external->sampleKind,
              graphics::ExternalBufferSampleKind::Unresolved);
    EXPECT_FLOAT_EQ(external->destination.width, 160.0f);
}

TEST(DisplayListWireTest, RejectsResolvedExternalBufferRuntimeState) {
    auto commands = std::make_shared<std::vector<graphics::DisplayCommand>>();
    graphics::DrawExternalBufferCommand external{};
    external.nodeId = 9;
    external.destination = {0.0f, 0.0f, 8.0f, 8.0f};
    external.bufferId = 11;
    external.contentRevision = 3;
    external.resourceKey = 0x1234u;
    external.sourceWidth = 8;
    external.sourceHeight = 8;
    external.stridePixels = 8;
    external.sampleKind = graphics::ExternalBufferSampleKind::ArgbPixels;
    commands->emplace_back(external);
    const graphics::DisplayList displayList{
        std::shared_ptr<const std::vector<graphics::DisplayCommand>>(commands)};
    EXPECT_EQ(graphics::encodeDisplayList(displayList).error,
              graphics::DisplayListWireError::UnsupportedCommand);
}

TEST(DisplayListWireTest, RejectsProcessLocalImagesWithoutResourceIdentity) {
    graphics::DisplayListBuilder builder;
    builder.drawImage({0.0f, 0.0f, 32.0f, 32.0f}, 0x1234u,
                      32, 32, 32, 1.0f, 4.0f, 2.0f, false);
    const auto encoded = graphics::encodeDisplayList(builder.build());
    EXPECT_FALSE(encoded);
    EXPECT_EQ(encoded.error, graphics::DisplayListWireError::UnsupportedCommand);
}

TEST(DisplayListWireTest, RejectsTruncationTrailingBytesAndUnknownVersion) {
    graphics::DisplayListBuilder builder;
    builder.drawText({1.0f, 2.0f}, "safe", {255, 255, 255, 255}, 14.0f,
                     graphics::FontFamily::Interface);
    const auto encoded = graphics::encodeDisplayList(builder.build());
    ASSERT_TRUE(encoded);

    std::vector<uint8_t> truncated = encoded.bytes;
    truncated.pop_back();
    EXPECT_EQ(graphics::decodeDisplayList(truncated).error,
              graphics::DisplayListWireError::InvalidData);

    std::vector<uint8_t> trailing = encoded.bytes;
    trailing.push_back(0);
    EXPECT_EQ(graphics::decodeDisplayList(trailing).error,
              graphics::DisplayListWireError::InvalidData);

    std::vector<uint8_t> future = encoded.bytes;
    future[4] = 6;
    future[5] = 0;
    EXPECT_EQ(graphics::decodeDisplayList(future).error,
              graphics::DisplayListWireError::UnsupportedVersion);
}

TEST(DisplayListWireTest, EnforcesCallerPayloadLimit) {
    graphics::DisplayListBuilder builder;
    builder.drawText({1.0f, 2.0f}, std::string(256, 'x'),
                     {255, 255, 255, 255}, 14.0f,
                     graphics::FontFamily::Interface);

    const auto encoded = graphics::encodeDisplayList(builder.build(), 64);
    EXPECT_FALSE(encoded);
    EXPECT_EQ(encoded.error, graphics::DisplayListWireError::TooLarge);
}

TEST(DisplayListWireTest, RejectsUnbalancedStateCommands) {
    graphics::DisplayListBuilder builder;
    builder.restore();
    const auto encoded = graphics::encodeDisplayList(builder.build());
    ASSERT_TRUE(encoded);
    EXPECT_EQ(graphics::decodeDisplayList(encoded.bytes).error,
              graphics::DisplayListWireError::InvalidData);
}
