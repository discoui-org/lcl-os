#include "lcl-graphics/display_list_wire.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace lcl::graphics {
namespace {

enum class WireCommand : uint8_t {
    Save = 1,
    Restore,
    Concat,
    BeginLayer,
    EndLayer,
    ClipRect,
    ClipPath,
    ClearRect,
    BeginCachedLayer,
    EndCachedLayer,
    DrawCachedLayer,
    DrawPath,
    DrawText,
    DrawImage,
};

constexpr uint32_t kMaxWireCommands = 65536;
constexpr uint32_t kMaxPathElements = 65536;

class Writer {
public:
    explicit Writer(size_t limit) : m_limit(limit) {}

    bool ok() const noexcept { return m_ok; }
    void fail() noexcept { m_ok = false; }
    const std::vector<uint8_t>& bytes() const noexcept { return m_bytes; }
    std::vector<uint8_t> take() { return std::move(m_bytes); }

    void u8(uint8_t value) { append(&value, sizeof(value)); }
    void u16(uint16_t value) {
        u8(static_cast<uint8_t>(value));
        u8(static_cast<uint8_t>(value >> 8u));
    }
    void u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<uint8_t>(value >> shift));
        }
    }
    void u64(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<uint8_t>(value >> shift));
        }
    }
    void f32(float value) { u32(std::bit_cast<uint32_t>(value)); }
    void raw(std::span<const uint8_t> value) { append(value.data(), value.size()); }
    void string(const std::string& value) {
        if (value.size() > std::numeric_limits<uint32_t>::max()) {
            m_ok = false;
            return;
        }
        u32(static_cast<uint32_t>(value.size()));
        append(value.data(), value.size());
    }

private:
    void append(const void* data, size_t size) {
        if (size == 0) return;
        if (!m_ok || size > m_limit || m_bytes.size() > m_limit - size) {
            m_ok = false;
            return;
        }
        const auto* first = static_cast<const uint8_t*>(data);
        m_bytes.insert(m_bytes.end(), first, first + size);
    }

    size_t m_limit{0};
    bool m_ok{true};
    std::vector<uint8_t> m_bytes;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : m_bytes(bytes) {}

    bool ok() const noexcept { return m_ok; }
    bool empty() const noexcept { return m_offset == m_bytes.size(); }
    size_t remaining() const noexcept { return m_bytes.size() - m_offset; }

    uint8_t u8() {
        uint8_t value = 0;
        read(&value, sizeof(value));
        return value;
    }
    uint16_t u16() {
        uint16_t value = 0;
        value |= static_cast<uint16_t>(u8());
        value |= static_cast<uint16_t>(u8()) << 8u;
        return value;
    }
    uint32_t u32() {
        uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<uint32_t>(u8()) << shift;
        }
        return value;
    }
    uint64_t u64() {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<uint64_t>(u8()) << shift;
        }
        return value;
    }
    float f32() { return std::bit_cast<float>(u32()); }
    std::span<const uint8_t> span(size_t size) {
        if (!m_ok || size > remaining()) {
            m_ok = false;
            return {};
        }
        const auto result = m_bytes.subspan(m_offset, size);
        m_offset += size;
        return result;
    }
    std::string string() {
        const uint32_t size = u32();
        const auto data = span(size);
        if (!m_ok) return {};
        return std::string(reinterpret_cast<const char*>(data.data()), data.size());
    }

private:
    void read(void* destination, size_t size) {
        const auto data = span(size);
        if (m_ok) std::memcpy(destination, data.data(), size);
    }

    std::span<const uint8_t> m_bytes;
    size_t m_offset{0};
    bool m_ok{true};
};

bool finite(float value) { return std::isfinite(value); }

void writePoint(Writer& writer, const PointF& point) {
    writer.f32(point.x);
    writer.f32(point.y);
}

bool readPoint(Reader& reader, PointF& point) {
    point = {reader.f32(), reader.f32()};
    return reader.ok() && finite(point.x) && finite(point.y);
}

void writeRect(Writer& writer, const RectF& rect) {
    writer.f32(rect.x);
    writer.f32(rect.y);
    writer.f32(rect.width);
    writer.f32(rect.height);
}

bool readRect(Reader& reader, RectF& rect) {
    rect = {reader.f32(), reader.f32(), reader.f32(), reader.f32()};
    return reader.ok() && finite(rect.x) && finite(rect.y) &&
           finite(rect.width) && finite(rect.height);
}

void writeColor(Writer& writer, Color color) {
    writer.u8(color.r);
    writer.u8(color.g);
    writer.u8(color.b);
    writer.u8(color.a);
}

bool readColor(Reader& reader, Color& color) {
    color = {reader.u8(), reader.u8(), reader.u8(), reader.u8()};
    return reader.ok();
}

void writeMatrix(Writer& writer, const Matrix3& matrix) {
    writer.f32(matrix.a);
    writer.f32(matrix.b);
    writer.f32(matrix.c);
    writer.f32(matrix.d);
    writer.f32(matrix.tx);
    writer.f32(matrix.ty);
}

bool readMatrix(Reader& reader, Matrix3& matrix) {
    matrix = {reader.f32(), reader.f32(), reader.f32(),
              reader.f32(), reader.f32(), reader.f32()};
    return reader.ok() && finite(matrix.a) && finite(matrix.b) &&
           finite(matrix.c) && finite(matrix.d) &&
           finite(matrix.tx) && finite(matrix.ty);
}

void writePaint(Writer& writer, const Paint& paint) {
    writeColor(writer, paint.color);
    writer.u8(static_cast<uint8_t>(paint.style));
    writer.u8(static_cast<uint8_t>(paint.fillRule));
    writer.u8(static_cast<uint8_t>(paint.stroke.cap));
    writer.u8(static_cast<uint8_t>(paint.stroke.join));
    writer.u8(static_cast<uint8_t>(paint.stroke.scaling));
    writer.f32(paint.stroke.width);
    writer.f32(paint.stroke.miterLimit);
    writer.f32(paint.opacity);
}

bool readPaint(Reader& reader, Paint& paint) {
    if (!readColor(reader, paint.color)) return false;
    const uint8_t style = reader.u8();
    const uint8_t fillRule = reader.u8();
    const uint8_t cap = reader.u8();
    const uint8_t join = reader.u8();
    const uint8_t scaling = reader.u8();
    paint.stroke.width = reader.f32();
    paint.stroke.miterLimit = reader.f32();
    paint.opacity = reader.f32();
    if (!reader.ok() || style > static_cast<uint8_t>(PaintStyle::Stroke) ||
        fillRule > static_cast<uint8_t>(FillRule::EvenOdd) ||
        cap > static_cast<uint8_t>(StrokeCap::Square) ||
        join > static_cast<uint8_t>(StrokeJoin::Bevel) ||
        scaling > static_cast<uint8_t>(StrokeScaling::Hairline) ||
        !finite(paint.stroke.width) || !finite(paint.stroke.miterLimit) ||
        !finite(paint.opacity)) {
        return false;
    }
    paint.style = static_cast<PaintStyle>(style);
    paint.fillRule = static_cast<FillRule>(fillRule);
    paint.stroke.cap = static_cast<StrokeCap>(cap);
    paint.stroke.join = static_cast<StrokeJoin>(join);
    paint.stroke.scaling = static_cast<StrokeScaling>(scaling);
    return true;
}

void writePath(Writer& writer, const Path& path) {
    if (const PathPrimitive* primitive = path.primitive()) {
        writer.u8(1);
        writer.u8(static_cast<uint8_t>(primitive->kind));
        writeRect(writer, primitive->bounds);
        writer.f32(primitive->radiusX);
        writer.f32(primitive->radiusY);
        writer.f32(primitive->roundness);
        return;
    }

    writer.u8(0);
    const auto& elements = path.elements();
    if (elements.size() > kMaxPathElements) {
        writer.fail();
        return;
    }
    writer.u32(static_cast<uint32_t>(elements.size()));
    for (const PathElement& element : elements) {
        writer.u8(static_cast<uint8_t>(element.verb));
        writer.u8(element.largeArc ? 1 : 0);
        writer.u8(element.clockwise ? 1 : 0);
        writer.u8(0);
        writePoint(writer, element.p0);
        writePoint(writer, element.p1);
        writePoint(writer, element.p2);
        writer.f32(element.radiusX);
        writer.f32(element.radiusY);
        writer.f32(element.rotationRadians);
    }
}

bool readPath(Reader& reader, Path& path) {
    const uint8_t representation = reader.u8();
    if (representation == 1) {
        const uint8_t kind = reader.u8();
        RectF bounds{};
        if (!readRect(reader, bounds)) return false;
        const float radiusX = reader.f32();
        const float radiusY = reader.f32();
        const float roundness = reader.f32();
        if (!reader.ok() || kind > static_cast<uint8_t>(PathPrimitiveKind::TopRRect) ||
            !finite(radiusX) || !finite(radiusY) || !finite(roundness)) {
            return false;
        }
        switch (static_cast<PathPrimitiveKind>(kind)) {
        case PathPrimitiveKind::Rect:
            path.addRect(bounds);
            break;
        case PathPrimitiveKind::RRect:
            path.addRRect({bounds, radiusX, radiusY, roundness});
            break;
        case PathPrimitiveKind::Ellipse:
            path.addEllipse(bounds);
            break;
        case PathPrimitiveKind::TopRRect:
            path.addTopRRect(bounds, radiusX, roundness);
            break;
        }
        return true;
    }
    if (representation != 0) return false;

    const uint32_t count = reader.u32();
    if (!reader.ok() || count > kMaxPathElements) return false;
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t verb = reader.u8();
        const uint8_t largeArc = reader.u8();
        const uint8_t clockwise = reader.u8();
        const uint8_t reserved = reader.u8();
        PointF p0{}, p1{}, p2{};
        if (!readPoint(reader, p0) || !readPoint(reader, p1) ||
            !readPoint(reader, p2)) return false;
        const float radiusX = reader.f32();
        const float radiusY = reader.f32();
        const float rotation = reader.f32();
        if (!reader.ok() || verb > static_cast<uint8_t>(PathVerb::Close) ||
            largeArc > 1 || clockwise > 1 || reserved != 0 ||
            !finite(radiusX) || !finite(radiusY) || !finite(rotation)) {
            return false;
        }
        switch (static_cast<PathVerb>(verb)) {
        case PathVerb::MoveTo: path.moveTo(p0.x, p0.y); break;
        case PathVerb::LineTo: path.lineTo(p0.x, p0.y); break;
        case PathVerb::QuadTo: path.quadTo(p0.x, p0.y, p1.x, p1.y); break;
        case PathVerb::CubicTo:
            path.cubicTo(p0.x, p0.y, p1.x, p1.y, p2.x, p2.y);
            break;
        case PathVerb::Arc:
            path.arcTo(radiusX, radiusY, rotation, largeArc != 0,
                       clockwise != 0, p0.x, p0.y);
            break;
        case PathVerb::Close: path.close(); break;
        }
    }
    return true;
}

void appendCommand(Writer& output, WireCommand command, const Writer& payload) {
    output.u8(static_cast<uint8_t>(command));
    output.u8(0);
    output.u16(0);
    output.u32(static_cast<uint32_t>(payload.bytes().size()));
    output.raw(payload.bytes());
}

template <typename Enum>
bool enumAtMost(uint8_t value, Enum maximum) {
    static_assert(std::is_enum_v<Enum>);
    return value <= static_cast<uint8_t>(maximum);
}

} // namespace

DisplayListEncodeResult encodeDisplayList(const DisplayList& displayList,
                                          size_t maxBytes) {
    DisplayListEncodeResult result{};
    if (maxBytes < 12) {
        result.error = DisplayListWireError::TooLarge;
        return result;
    }
    const auto& commands = displayList.commands();
    if (commands.size() > kMaxWireCommands) {
        result.error = DisplayListWireError::TooLarge;
        return result;
    }

    Writer output(maxBytes);
    output.u32(kDisplayListWireMagic);
    output.u16(kDisplayListWireVersion);
    output.u16(0);
    output.u32(static_cast<uint32_t>(commands.size()));

    for (const DisplayCommand& command : commands) {
        Writer payload(maxBytes);
        WireCommand opcode{};
        bool supported = true;
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, SaveCommand>) {
                opcode = WireCommand::Save;
            } else if constexpr (std::is_same_v<T, RestoreCommand>) {
                opcode = WireCommand::Restore;
            } else if constexpr (std::is_same_v<T, ConcatCommand>) {
                opcode = WireCommand::Concat;
                writeMatrix(payload, item.transform);
            } else if constexpr (std::is_same_v<T, BeginLayerCommand>) {
                opcode = WireCommand::BeginLayer;
                payload.f32(item.opacity);
            } else if constexpr (std::is_same_v<T, EndLayerCommand>) {
                opcode = WireCommand::EndLayer;
            } else if constexpr (std::is_same_v<T, ClipRectCommand>) {
                opcode = WireCommand::ClipRect;
                writeRect(payload, item.rect);
            } else if constexpr (std::is_same_v<T, ClipPathCommand>) {
                opcode = WireCommand::ClipPath;
                payload.u8(static_cast<uint8_t>(item.fillRule));
                writePath(payload, item.path);
            } else if constexpr (std::is_same_v<T, ClearRectCommand>) {
                opcode = WireCommand::ClearRect;
                writeRect(payload, item.rect);
                writeColor(payload, item.color);
            } else if constexpr (std::is_same_v<T, BeginCachedLayerCommand>) {
                opcode = WireCommand::BeginCachedLayer;
                payload.u64(item.id);
                writeRect(payload, item.sourceBounds);
                payload.u8(item.updateBounds.has_value() ? 1 : 0);
                if (item.updateBounds) writeRect(payload, *item.updateBounds);
            } else if constexpr (std::is_same_v<T, EndCachedLayerCommand>) {
                opcode = WireCommand::EndCachedLayer;
            } else if constexpr (std::is_same_v<T, DrawCachedLayerCommand>) {
                opcode = WireCommand::DrawCachedLayer;
                payload.u64(item.id);
                writeRect(payload, item.destination);
                payload.f32(item.opacity);
            } else if constexpr (std::is_same_v<T, DrawPathCommand>) {
                opcode = WireCommand::DrawPath;
                writePaint(payload, item.paint);
                writePath(payload, item.path);
            } else if constexpr (std::is_same_v<T, DrawTextCommand>) {
                opcode = WireCommand::DrawText;
                writePoint(payload, item.origin);
                writeColor(payload, item.color);
                payload.f32(item.fontSize);
                payload.u8(static_cast<uint8_t>(item.fontFamily));
                payload.string(item.text);
            } else if constexpr (std::is_same_v<T, DrawImageCommand>) {
                if (item.resourceId == 0 || item.contentRevision == 0 ||
                    item.sourceWidth <= 0 || item.sourceHeight <= 0 ||
                    item.stridePixels < item.sourceWidth) {
                    supported = false;
                    return;
                }
                opcode = WireCommand::DrawImage;
                writeRect(payload, item.destination);
                payload.u64(item.resourceId);
                payload.u64(item.contentRevision);
                payload.u32(static_cast<uint32_t>(item.sourceWidth));
                payload.u32(static_cast<uint32_t>(item.sourceHeight));
                payload.u32(static_cast<uint32_t>(item.stridePixels));
                payload.u8(item.opaque ? 1 : 0);
                payload.f32(item.opacity);
                payload.f32(item.cornerRadius);
                payload.f32(item.cornerRoundness);
                payload.u8(item.squareTopCorners ? 1 : 0);
            }
        }, command);

        if (!supported) {
            result.error = DisplayListWireError::UnsupportedCommand;
            return result;
        }
        if (!payload.ok() || payload.bytes().size() > std::numeric_limits<uint32_t>::max()) {
            result.error = DisplayListWireError::TooLarge;
            return result;
        }
        appendCommand(output, opcode, payload);
        if (!output.ok()) {
            result.error = DisplayListWireError::TooLarge;
            return result;
        }
    }

    result.bytes = output.take();
    return result;
}

DisplayListDecodeResult decodeDisplayList(std::span<const uint8_t> bytes,
                                          size_t maxBytes) {
    DisplayListDecodeResult result{};
    if (bytes.size() > maxBytes) {
        result.error = DisplayListWireError::TooLarge;
        return result;
    }

    Reader input(bytes);
    const uint32_t magic = input.u32();
    const uint16_t version = input.u16();
    const uint16_t reserved = input.u16();
    const uint32_t commandCount = input.u32();
    if (!input.ok() || magic != kDisplayListWireMagic || reserved != 0 ||
        commandCount > kMaxWireCommands) {
        result.error = DisplayListWireError::InvalidData;
        return result;
    }
    if (version != kDisplayListWireVersion) {
        result.error = DisplayListWireError::UnsupportedVersion;
        return result;
    }

    DisplayListBuilder builder;
    uint32_t saveDepth = 0;
    uint32_t layerDepth = 0;
    bool cachedLayerActive = false;
    for (uint32_t index = 0; index < commandCount; ++index) {
        const uint8_t rawOpcode = input.u8();
        const uint8_t reserved0 = input.u8();
        const uint16_t reserved1 = input.u16();
        const uint32_t payloadSize = input.u32();
        const auto payloadBytes = input.span(payloadSize);
        if (!input.ok() || reserved0 != 0 || reserved1 != 0 ||
            rawOpcode < static_cast<uint8_t>(WireCommand::Save) ||
            rawOpcode > static_cast<uint8_t>(WireCommand::DrawImage)) {
            result.error = DisplayListWireError::InvalidData;
            return result;
        }
        Reader payload(payloadBytes);
        bool valid = true;
        switch (static_cast<WireCommand>(rawOpcode)) {
        case WireCommand::Save:
            ++saveDepth;
            builder.save();
            break;
        case WireCommand::Restore:
            valid = saveDepth > 0;
            if (valid) {
                --saveDepth;
                builder.restore();
            }
            break;
        case WireCommand::Concat: {
            Matrix3 matrix{};
            valid = readMatrix(payload, matrix);
            if (valid) builder.concat(matrix);
            break;
        }
        case WireCommand::BeginLayer: {
            const float opacity = payload.f32();
            valid = payload.ok() && finite(opacity);
            if (valid) {
                ++layerDepth;
                builder.beginLayer(opacity);
            }
            break;
        }
        case WireCommand::EndLayer:
            valid = layerDepth > 0;
            if (valid) {
                --layerDepth;
                builder.endLayer();
            }
            break;
        case WireCommand::ClipRect: {
            RectF rect{};
            valid = readRect(payload, rect);
            if (valid) builder.clipRect(rect);
            break;
        }
        case WireCommand::ClipPath: {
            const uint8_t fillRule = payload.u8();
            Path path;
            valid = enumAtMost(fillRule, FillRule::EvenOdd) && readPath(payload, path);
            if (valid) builder.clipPath(path, static_cast<FillRule>(fillRule));
            break;
        }
        case WireCommand::ClearRect: {
            RectF rect{};
            Color color{};
            valid = readRect(payload, rect) && readColor(payload, color);
            if (valid) builder.clearRect(rect, color);
            break;
        }
        case WireCommand::BeginCachedLayer: {
            const uint64_t id = payload.u64();
            RectF bounds{};
            valid = readRect(payload, bounds);
            const uint8_t hasUpdate = payload.u8();
            valid = valid && payload.ok() && hasUpdate <= 1;
            RectF update{};
            if (valid && hasUpdate) valid = readRect(payload, update);
            valid = valid && !cachedLayerActive;
            if (valid) {
                cachedLayerActive = true;
                if (hasUpdate) builder.beginCachedLayerUpdate(id, bounds, update);
                else builder.beginCachedLayer(id, bounds);
            }
            break;
        }
        case WireCommand::EndCachedLayer:
            valid = cachedLayerActive;
            if (valid) {
                cachedLayerActive = false;
                builder.endCachedLayer();
            }
            break;
        case WireCommand::DrawCachedLayer: {
            const uint64_t id = payload.u64();
            RectF destination{};
            valid = readRect(payload, destination);
            const float opacity = payload.f32();
            valid = valid && payload.ok() && finite(opacity);
            if (valid) builder.drawCachedLayer(id, destination, opacity);
            break;
        }
        case WireCommand::DrawPath: {
            Paint paint{};
            Path path;
            valid = readPaint(payload, paint) && readPath(payload, path);
            if (valid) builder.drawPath(path, paint);
            break;
        }
        case WireCommand::DrawText: {
            PointF origin{};
            Color color{};
            valid = readPoint(payload, origin) && readColor(payload, color);
            const float fontSize = payload.f32();
            const uint8_t family = payload.u8();
            std::string text = payload.string();
            valid = valid && payload.ok() && finite(fontSize) &&
                    enumAtMost(family, FontFamily::Monospace);
            if (valid) {
                builder.drawText(origin, std::move(text), color, fontSize,
                                 static_cast<FontFamily>(family));
            }
            break;
        }
        case WireCommand::DrawImage: {
            RectF destination{};
            valid = readRect(payload, destination);
            const uint64_t resourceId = payload.u64();
            const uint64_t contentRevision = payload.u64();
            const uint32_t width = payload.u32();
            const uint32_t height = payload.u32();
            const uint32_t stride = payload.u32();
            const uint8_t opaque = payload.u8();
            const float opacity = payload.f32();
            const float cornerRadius = payload.f32();
            const float cornerRoundness = payload.f32();
            const uint8_t squareTopCorners = payload.u8();
            valid = valid && payload.ok() && resourceId != 0 &&
                contentRevision != 0 && width > 0 && height > 0 &&
                stride >= width && opaque <= 1 && squareTopCorners <= 1 &&
                finite(opacity) && finite(cornerRadius) &&
                finite(cornerRoundness);
            if (valid) {
                builder.drawImage(
                    destination, 0, static_cast<int>(width),
                    static_cast<int>(height), static_cast<int>(stride),
                    opacity, cornerRadius, cornerRoundness,
                    squareTopCorners != 0, resourceId, contentRevision,
                    opaque != 0);
            }
            break;
        }
        }

        if (!valid || !payload.ok() || !payload.empty()) {
            result.error = DisplayListWireError::InvalidData;
            return result;
        }
    }

    if (!input.ok() || !input.empty() || saveDepth != 0 ||
        layerDepth != 0 || cachedLayerActive) {
        result.error = DisplayListWireError::InvalidData;
        return result;
    }
    result.displayList = builder.build();
    return result;
}

} // namespace lcl::graphics
