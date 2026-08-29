#include "core/ipc/lcl_protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

namespace lcl::protocol {
namespace {

constexpr size_t kMaxQueuedBytesPerSocket = 4u * 1024u * 1024u;
constexpr size_t kMaxEffectRegions = 4096;
constexpr size_t kMaxEffectFilters = 16384;

class Writer {
  public:
    void u8(uint8_t value) { m_bytes.push_back(value); }
    void u16(uint16_t value) {
        u8(static_cast<uint8_t>(value));
        u8(static_cast<uint8_t>(value >> 8));
    }
    void u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<uint8_t>(value >> shift));
        }
    }
    void u64(uint64_t value) {
        u32(static_cast<uint32_t>(value));
        u32(static_cast<uint32_t>(value >> 32));
    }
    void i32(int32_t value) { u32(std::bit_cast<uint32_t>(value)); }
    void f32(float value) { u32(std::bit_cast<uint32_t>(value)); }
    void fixed(const char* value, size_t size) {
        m_bytes.insert(m_bytes.end(), value, value + size);
    }
    std::vector<uint8_t> take() { return std::move(m_bytes); }

  private:
    std::vector<uint8_t> m_bytes;
};

class Reader {
  public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
    bool u8(uint8_t& value) {
        if (m_offset >= m_size)
            return false;
        value = m_data[m_offset++];
        return true;
    }
    bool u16(uint16_t& value) {
        uint8_t a = 0, b = 0;
        if (!u8(a) || !u8(b))
            return false;
        value = static_cast<uint16_t>(a) | (static_cast<uint16_t>(b) << 8);
        return true;
    }
    bool u32(uint32_t& value) {
        uint8_t b[4]{};
        if (!u8(b[0]) || !u8(b[1]) || !u8(b[2]) || !u8(b[3]))
            return false;
        value = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                (static_cast<uint32_t>(b[2]) << 16) |
                (static_cast<uint32_t>(b[3]) << 24);
        return true;
    }
    bool u64(uint64_t& value) {
        uint32_t low = 0, high = 0;
        if (!u32(low) || !u32(high))
            return false;
        value = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
        return true;
    }
    bool i32(int32_t& value) {
        uint32_t raw = 0;
        if (!u32(raw))
            return false;
        value = std::bit_cast<int32_t>(raw);
        return true;
    }
    bool f32(float& value) {
        uint32_t raw = 0;
        if (!u32(raw))
            return false;
        value = std::bit_cast<float>(raw);
        return true;
    }
    bool fixed(char* value, size_t size) {
        if (size > m_size - m_offset)
            return false;
        std::memcpy(value, m_data + m_offset, size);
        m_offset += size;
        return true;
    }
    bool done() const { return m_offset == m_size; }

  private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_offset{0};
};

template <typename T>
bool loadNative(const void* payload, size_t payloadSize, size_t offset,
                T& value) {
    if (!payload || offset > payloadSize || sizeof(T) > payloadSize - offset)
        return false;
    std::memcpy(&value, static_cast<const uint8_t*>(payload) + offset,
                sizeof(T));
    return true;
}

template <typename T>
void appendNative(std::vector<uint8_t>& payload, const T& value) {
    const size_t offset = payload.size();
    payload.resize(offset + sizeof(T));
    std::memcpy(payload.data() + offset, &value, sizeof(T));
}

bool validString(const char* value, size_t size) {
    return std::memchr(value, '\0', size) != nullptr;
}
bool validScale(float value) {
    return std::isfinite(value) && value >= 0.5f && value <= 4.0f;
}
bool validFloat(float value) { return std::isfinite(value); }
bool validResizeConstraint(float value) {
    return validFloat(value) && value >= 0.0f && value <= 16384.0f;
}
bool validUnit(float value) {
    return validFloat(value) && value >= 0.0f && value <= 1.0f;
}
bool validDecoration(LCLDecorationMode value) {
    return value >= LCLDecorationMode::SSD && value <= LCLDecorationMode::None;
}
bool validConfigureResizeReason(LCLConfigureResizeReason value) {
    return value >= LCLConfigureResizeReason::Initial &&
           value <= LCLConfigureResizeReason::WindowStateTransition;
}
bool validLayer(LCLWindowLayer value) {
    return value >= LCLWindowLayer::Bottom && value <= LCLWindowLayer::TopMost;
}
bool validAction(LCLWindowAction value) {
    return value >= LCLWindowAction::BeginDrag && value <= LCLWindowAction::Close;
}
bool validSceneVisibility(LCLSceneVisibility value) {
    return value >= LCLSceneVisibility::Visible && value <= LCLSceneVisibility::Closing;
}
bool validShellDeltaKind(LCLShellStateDeltaKind value) {
    return value >= LCLShellStateDeltaKind::SceneAdded &&
           value <= LCLShellStateDeltaKind::FocusChanged;
}
bool validSystemSurfaceKind(LCLSystemSurfaceKind value) {
    return value >= LCLSystemSurfaceKind::None &&
           value <= LCLSystemSurfaceKind::HomeScreen;
}
bool validPopupRole(LCLPopupRole value) {
    return value == LCLPopupRole::Transient;
}
bool validAttachedRole(LCLAttachedSurfaceRole value) {
    return value >= LCLAttachedSurfaceRole::Frame &&
           value <= LCLAttachedSurfaceRole::Adornment;
}
bool validFilter(FilterType value) {
    return value >= FilterType::None && value <= FilterType::Tint;
}
bool validProfile(uint8_t value) {
    return value <= static_cast<uint8_t>(GlassProfile::Dense);
}
bool validSource(EffectSourceType value) {
    return value >= EffectSourceType::Backdrop &&
           value <= EffectSourceType::Foreground;
}
bool validBlend(EffectBlendMode value) {
    return value >= EffectBlendMode::Normal && value <= EffectBlendMode::Plus;
}
bool validBoundsPolicy(EffectBoundsPolicy value) {
    return value >= EffectBoundsPolicy::Local &&
           value <= EffectBoundsPolicy::OuterSurface;
}
bool validOpcode(LCLOpcode value) {
    switch (value) {
    case LCLOpcode::SurfaceCreate:
    case LCLOpcode::SurfaceDestroy:
    case LCLOpcode::ConfigureBounds:
    case LCLOpcode::FramePresented:
    case LCLOpcode::FrameDiscarded:
    case LCLOpcode::InputEvent:
    case LCLOpcode::AckResponse:
    case LCLOpcode::SetDecorationMode:
    case LCLOpcode::SetWindowLayer:
    case LCLOpcode::SetReservedZone:
    case LCLOpcode::SetEffectGraph:
    case LCLOpcode::ClearEffectGraph:
    case LCLOpcode::BeginWindowMove:
    case LCLOpcode::RequestSurfaceClose:
    case LCLOpcode::SetInsetBorder:
    case LCLOpcode::SetWindowCornerRadius:
    case LCLOpcode::RequestWindowAction:
    case LCLOpcode::SubscribeShellState:
    case LCLOpcode::ShellStateSnapshot:
    case LCLOpcode::ShellStateDelta:
    case LCLOpcode::SetSystemSurfaceKind:
    case LCLOpcode::SetWindowCornerStyle:
    case LCLOpcode::SetEdgeToEdge:
    case LCLOpcode::PopupSurfaceCreate:
    case LCLOpcode::BeginLaunchPlaceholder:
    case LCLOpcode::ResolveLaunchPlaceholder:
    case LCLOpcode::CancelLaunchPlaceholder:
    case LCLOpcode::LaunchIconVisibility:
    case LCLOpcode::LaunchIconVisibilityAck:
    case LCLOpcode::AttachedSurfaceCreate:
    case LCLOpcode::RequestManagedWindowAction:
    case LCLOpcode::SurfaceProducerGrant:
        return true;
    }
    return false;
}

bool validShellScene(const LCLMsgShellScene& scene) {
    return scene.sceneId != 0 && validFloat(scene.x) && validFloat(scene.y) &&
           validFloat(scene.width) && scene.width >= 0.0f &&
           validFloat(scene.height) && scene.height >= 0.0f &&
           validSceneVisibility(scene.visibility) &&
           validDecoration(scene.decorationMode) &&
           scene.edgeToEdge <= 1 &&
           validString(scene.appId, sizeof(scene.appId)) &&
           validString(scene.title, sizeof(scene.title));
}

void encodeShellScene(Writer& out, const LCLMsgShellScene& scene) {
    out.u64(scene.sceneId);
    out.u64(scene.appInstanceId);
    out.u32(scene.windowId);
    out.i32(scene.clientPid);
    out.u32(scene.displayId);
    out.u32(scene.workspaceId);
    out.f32(scene.x);
    out.f32(scene.y);
    out.f32(scene.width);
    out.f32(scene.height);
    out.u8(static_cast<uint8_t>(scene.visibility));
    out.u32(static_cast<uint32_t>(scene.decorationMode));
    out.u8(scene.edgeToEdge);
    out.fixed(scene.appId, sizeof(scene.appId));
    out.fixed(scene.title, sizeof(scene.title));
}

bool decodeShellScene(Reader& in, LCLMsgShellScene& scene, bool requireIdentity = true) {
    uint8_t visibility = 0;
    uint32_t decorationMode = 0;
    if (!in.u64(scene.sceneId) || !in.u64(scene.appInstanceId) ||
        !in.u32(scene.windowId) || !in.i32(scene.clientPid) ||
        !in.u32(scene.displayId) || !in.u32(scene.workspaceId) ||
        !in.f32(scene.x) || !in.f32(scene.y) || !in.f32(scene.width) ||
        !in.f32(scene.height) || !in.u8(visibility) ||
        !in.u32(decorationMode) || !in.u8(scene.edgeToEdge) ||
        !in.fixed(scene.appId, sizeof(scene.appId)) ||
        !in.fixed(scene.title, sizeof(scene.title)))
        return false;
    scene.visibility = static_cast<LCLSceneVisibility>(visibility);
    scene.decorationMode = static_cast<LCLDecorationMode>(decorationMode);
    if (!validFloat(scene.x) || !validFloat(scene.y) ||
        !validFloat(scene.width) || scene.width < 0.0f ||
        !validFloat(scene.height) || scene.height < 0.0f ||
        !validSceneVisibility(scene.visibility) ||
        !validDecoration(scene.decorationMode) ||
        scene.edgeToEdge > 1 ||
        !validString(scene.appId, sizeof(scene.appId)) ||
        !validString(scene.title, sizeof(scene.title)))
        return false;
    return !requireIdentity || scene.sceneId != 0;
}

void encodeFilter(Writer& out, const FilterOp& op) {
    out.u8(static_cast<uint8_t>(op.type));
    out.f32(op.value);
    out.u8(op.profile);
    out.u8(op.reserved0);
    out.u16(op.reserved1);
    for (float param : op.params)
        out.f32(param);
}
bool decodeFilter(Reader& in, FilterOp& op) {
    uint8_t type = 0;
    if (!in.u8(type) || !in.f32(op.value) || !in.u8(op.profile) ||
        !in.u8(op.reserved0) || !in.u16(op.reserved1))
        return false;
    op.type = static_cast<FilterType>(type);
    for (float& param : op.params)
        if (!in.f32(param))
            return false;
    return validFilter(op.type) && validProfile(op.profile) &&
           validFloat(op.value) &&
           std::all_of(std::begin(op.params), std::end(op.params), validFloat);
}
void encodeRegion(Writer& out, const EffectRegion& region) {
    out.f32(region.x);
    out.f32(region.y);
    out.f32(region.width);
    out.f32(region.height);
    out.f32(region.cornerRadius);
    out.f32(region.cornerRoundness);
    out.u8(static_cast<uint8_t>(region.boundsPolicy));
    out.u8(static_cast<uint8_t>(region.source));
    out.u8(static_cast<uint8_t>(region.blendMode));
    out.u16(region.filterCount);
    out.u32(region.filterOffset);
    out.f32(region.opacity);
}
bool decodeRegion(Reader& in, EffectRegion& region) {
    uint8_t boundsPolicy = 0, source = 0, blend = 0;
    if (!in.f32(region.x) || !in.f32(region.y) || !in.f32(region.width) ||
        !in.f32(region.height) || !in.f32(region.cornerRadius) ||
        !in.f32(region.cornerRoundness) ||
        !in.u8(boundsPolicy) ||
        !in.u8(source) || !in.u8(blend) || !in.u16(region.filterCount) ||
        !in.u32(region.filterOffset) || !in.f32(region.opacity))
        return false;
    region.boundsPolicy = static_cast<EffectBoundsPolicy>(boundsPolicy);
    region.source = static_cast<EffectSourceType>(source);
    region.blendMode = static_cast<EffectBlendMode>(blend);
    return validFloat(region.x) && validFloat(region.y) &&
           validFloat(region.width) && region.width > 0.0f &&
           validFloat(region.height) && region.height > 0.0f &&
           validFloat(region.cornerRadius) && region.cornerRadius >= 0.0f &&
           validFloat(region.cornerRoundness) && region.cornerRoundness >= 2.0f &&
           region.cornerRoundness <= 8.0f &&
           validBoundsPolicy(region.boundsPolicy) &&
           validSource(region.source) && validBlend(region.blendMode) &&
           validUnit(region.opacity);
}

bool encodePayload(LCLOpcode opcode, const void* payload, size_t size,
                   Writer& out) {
#define LOAD_ONE(Type, name)                                         \
    Type name{};                                                     \
    if (!loadNative(payload, size, 0, name) || size != sizeof(Type)) \
    return false
    switch (opcode) {
    case LCLOpcode::SurfaceCreate: {
        LOAD_ONE(LCLMsgSurfaceCreate, msg);
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (msg.surfaceId == 0 || !validFloat(msg.x) || !validFloat(msg.y) ||
            !validFloat(msg.width) || msg.width <= 0.0f ||
            !validFloat(msg.height) || msg.height <= 0.0f ||
            !validString(msg.title, sizeof(msg.title)) ||
            !validString(msg.appId, sizeof(msg.appId)) || msg.appId[0] == '\0' ||
            msg.hasLaunchOrigin > 1 ||
            !validFloat(msg.launchOriginX) || !validFloat(msg.launchOriginY) ||
            !validFloat(msg.launchOriginWidth) || !validFloat(msg.launchOriginHeight) ||
            !validFloat(msg.launchOriginCornerRadius) ||
            !validResizeConstraint(msg.resizeBaseWidth) ||
            !validResizeConstraint(msg.resizeBaseHeight) ||
            !validResizeConstraint(msg.resizeWidthIncrement) ||
            !validResizeConstraint(msg.resizeHeightIncrement) ||
            msg.launchToken >= kLaunchTokenLimit ||
            (msg.hasLaunchOrigin &&
             (msg.launchOriginWidth <= 0.0f || msg.launchOriginHeight <= 0.0f ||
              msg.launchOriginCornerRadius < 0.0f)))
            return false;
        out.u32(msg.surfaceId);
        out.f32(msg.x);
        out.f32(msg.y);
        out.f32(msg.width);
        out.f32(msg.height);
        out.fixed(msg.title, sizeof(msg.title));
        out.fixed(msg.appId, sizeof(msg.appId));
        out.u8(msg.hasLaunchOrigin);
        out.f32(msg.launchOriginX);
        out.f32(msg.launchOriginY);
        out.f32(msg.launchOriginWidth);
        out.f32(msg.launchOriginHeight);
        out.f32(msg.launchOriginCornerRadius);
        out.u64(msg.launchToken);
        out.u64(msg.appInstanceId);
        out.f32(msg.resizeBaseWidth);
        out.f32(msg.resizeBaseHeight);
        out.f32(msg.resizeWidthIncrement);
        out.f32(msg.resizeHeightIncrement);
        return true;
    }
    case LCLOpcode::PopupSurfaceCreate: {
        LOAD_ONE(LCLMsgPopupSurfaceCreate, msg);
        if (msg.surfaceId == 0 || msg.parentSurfaceId == 0 ||
            msg.surfaceId == msg.parentSurfaceId || !validFloat(msg.x) ||
            !validFloat(msg.y) || !validFloat(msg.width) || msg.width <= 0.0f ||
            !validFloat(msg.height) || msg.height <= 0.0f || !validPopupRole(msg.role))
            return false;
        out.u32(msg.surfaceId);
        out.u32(msg.parentSurfaceId);
        out.u32(static_cast<uint32_t>(msg.role));
        out.f32(msg.x);
        out.f32(msg.y);
        out.f32(msg.width);
        out.f32(msg.height);
        return true;
    }
    case LCLOpcode::AttachedSurfaceCreate: {
        LOAD_ONE(LCLMsgAttachedSurfaceCreate, msg);
        if (msg.surfaceId == 0 || msg.targetWindowId == 0 ||
            !validAttachedRole(msg.role) || !validFloat(msg.x) ||
            !validFloat(msg.y) || !validFloat(msg.width) || msg.width <= 0.0f ||
            !validFloat(msg.height) || msg.height <= 0.0f ||
            msg.followParentWidth > 1 || msg.followParentHeight > 1 ||
            msg.acceptsInput > 1)
            return false;
        out.u32(msg.surfaceId);
        out.u32(msg.targetWindowId);
        out.u32(static_cast<uint32_t>(msg.role));
        out.f32(msg.x);
        out.f32(msg.y);
        out.f32(msg.width);
        out.f32(msg.height);
        out.u8(msg.followParentWidth);
        out.u8(msg.followParentHeight);
        out.u8(msg.acceptsInput);
        return true;
    }
    case LCLOpcode::SurfaceDestroy: {
        LOAD_ONE(LCLMsgSurfaceDestroy, msg);
        out.u32(msg.surfaceId);
        return msg.surfaceId > 0;
    }
    case LCLOpcode::ConfigureBounds: {
        LOAD_ONE(LCLMsgConfigureBounds, msg);
        if (msg.surfaceId == 0 || !validFloat(msg.x) || !validFloat(msg.y) ||
            !validFloat(msg.width) || msg.width <= 0.0f ||
            !validFloat(msg.height) || msg.height <= 0.0f ||
            !validFloat(msg.backingWidth) || !validFloat(msg.backingHeight) ||
            msg.backingWidth < msg.width || msg.backingHeight < msg.height ||
            msg.configureSerial == 0 || msg.isFocused > 1 || !validScale(msg.bufferScale) ||
            !validConfigureResizeReason(msg.resizeReason) ||
            !validString(msg.title, sizeof(msg.title)))
            return false;
        out.u32(msg.surfaceId);
        out.u64(msg.configureSerial);
        out.u64(msg.geometryGeneration);
        out.f32(msg.x);
        out.f32(msg.y);
        out.f32(msg.width);
        out.f32(msg.height);
        out.f32(msg.backingWidth);
        out.f32(msg.backingHeight);
        out.u8(msg.isFocused);
        out.fixed(msg.title, sizeof(msg.title));
        out.f32(msg.bufferScale);
        out.u8(static_cast<uint8_t>(msg.resizeReason));
        return true;
    }
    case LCLOpcode::FramePresented: {
        LOAD_ONE(LCLMsgFramePresented, msg);
        if (msg.surfaceId == 0 || msg.configureSerial == 0 ||
            msg.frameSerial == 0 || msg.displaySequence == 0 ||
            msg.timestampNs == 0 ||
            msg.refreshIntervalNs == 0)
            return false;
        out.u32(msg.surfaceId);
        out.u64(msg.configureSerial);
        out.u64(msg.frameSerial);
        out.u64(msg.geometryGeneration);
        out.u64(msg.displaySequence);
        out.u64(msg.timestampNs);
        out.u64(msg.refreshIntervalNs);
        return true;
    }
    case LCLOpcode::FrameDiscarded: {
        LOAD_ONE(LCLMsgFrameDiscarded, msg);
        if (msg.surfaceId == 0 || msg.configureSerial == 0 ||
            msg.frameSerial == 0) return false;
        out.u32(msg.surfaceId);
        out.u64(msg.configureSerial);
        out.u64(msg.frameSerial);
        out.u64(msg.geometryGeneration);
        out.u32(static_cast<uint32_t>(msg.reason));
        return true;
    }
    case LCLOpcode::AckResponse: {
        LOAD_ONE(LCLMsgAckResponse, msg);
        if (!validString(msg.message, sizeof(msg.message)))
            return false;
        out.u32(msg.status);
        out.fixed(msg.message, sizeof(msg.message));
        return true;
    }
    case LCLOpcode::InputEvent: {
        LOAD_ONE(LCLMsgInputEvent, msg);
        if (msg.surfaceId == 0 ||
            msg.type < static_cast<uint32_t>(LCLInputEventType::KeyDown) ||
            msg.type > static_cast<uint32_t>(LCLInputEventType::PointerCancel) ||
            msg.pressed > 1 ||
            !validFloat(msg.x) || !validFloat(msg.y) || !validFloat(msg.deltaX) || !validFloat(msg.deltaY))
            return false;
        out.u32(msg.surfaceId);
        out.u32(msg.type);
        out.u32(msg.key);
        out.u8(msg.pressed);
        out.u8(msg.modifiers);
        out.u8(msg.source);
        out.u32(msg.pointerId);
        out.u32(msg.codepoint);
        out.f32(msg.x);
        out.f32(msg.y);
        out.f32(msg.deltaX);
        out.f32(msg.deltaY);
        return true;
    }
    case LCLOpcode::SetDecorationMode: {
        LOAD_ONE(LCLMsgSetDecorationMode, msg);
        out.u32(msg.surfaceId);
        out.u32(static_cast<uint32_t>(msg.mode));
        return msg.surfaceId > 0 && validDecoration(msg.mode);
    }
    case LCLOpcode::SetEdgeToEdge: {
        LOAD_ONE(LCLMsgSetEdgeToEdge, msg);
        out.u32(msg.surfaceId);
        out.u8(msg.enabled);
        return msg.surfaceId > 0 && msg.enabled <= 1;
    }
    case LCLOpcode::SetWindowLayer: {
        LOAD_ONE(LCLMsgSetWindowLayer, msg);
        out.u32(msg.surfaceId);
        out.u32(static_cast<uint32_t>(msg.layer));
        out.u8(msg.unfocusable);
        return msg.surfaceId > 0 && validLayer(msg.layer) && msg.unfocusable <= 1;
    }
    case LCLOpcode::SetReservedZone: {
        LOAD_ONE(LCLMsgSetReservedZone, msg);
        out.u32(msg.surfaceId);
        out.f32(msg.top);
        out.f32(msg.bottom);
        out.f32(msg.left);
        out.f32(msg.right);
        return msg.surfaceId > 0 && validFloat(msg.top) && msg.top >= 0.0f &&
               validFloat(msg.bottom) && msg.bottom >= 0.0f &&
               validFloat(msg.left) && msg.left >= 0.0f &&
               validFloat(msg.right) && msg.right >= 0.0f;
    }
    case LCLOpcode::SetEffectGraph: {
        LCLMsgSetEffectGraphHeader graph{};
        if (!loadNative(payload, size, 0, graph) || graph.surfaceId == 0 ||
            graph.regionCount > kMaxEffectRegions ||
            graph.filterCount > kMaxEffectFilters)
            return false;
        const size_t expected =
            sizeof(graph) +
            static_cast<size_t>(graph.regionCount) * sizeof(EffectRegion) +
            static_cast<size_t>(graph.filterCount) * sizeof(FilterOp);
        if (size != expected)
            return false;
        out.u32(graph.surfaceId);
        out.u32(graph.regionCount);
        out.u32(graph.filterCount);
        size_t offset = sizeof(graph);
        for (uint32_t i = 0; i < graph.regionCount;
             ++i, offset += sizeof(EffectRegion)) {
            EffectRegion region{};
            if (!loadNative(payload, size, offset, region))
                return false;
            if (region.filterOffset > graph.filterCount ||
                region.filterCount > graph.filterCount - region.filterOffset ||
                !validFloat(region.cornerRadius) || region.cornerRadius < 0.0f ||
                !validFloat(region.cornerRoundness) || region.cornerRoundness < 2.0f ||
                region.cornerRoundness > 8.0f ||
                !validBoundsPolicy(region.boundsPolicy) ||
                !validUnit(region.opacity) || !validSource(region.source) ||
                !validBlend(region.blendMode) || !validFloat(region.x) ||
                !validFloat(region.y) || !validFloat(region.width) ||
                region.width <= 0.0f || !validFloat(region.height) ||
                region.height <= 0.0f)
                return false;
            encodeRegion(out, region);
        }
        for (uint32_t i = 0; i < graph.filterCount;
             ++i, offset += sizeof(FilterOp)) {
            FilterOp op{};
            if (!loadNative(payload, size, offset, op) || !validFilter(op.type) ||
                !validProfile(op.profile) || !validFloat(op.value) ||
                !std::all_of(std::begin(op.params), std::end(op.params), validFloat))
                return false;
            encodeFilter(out, op);
        }
        return true;
    }
    case LCLOpcode::ClearEffectGraph: {
        LOAD_ONE(LCLMsgClearEffectGraph, msg);
        out.u32(msg.surfaceId);
        return msg.surfaceId > 0;
    }
    case LCLOpcode::BeginWindowMove: {
        LOAD_ONE(LCLMsgBeginWindowMove, msg);
        out.u32(msg.surfaceId);
        out.f32(msg.localX);
        out.f32(msg.localY);
        return msg.surfaceId > 0 && validFloat(msg.localX) &&
               validFloat(msg.localY);
    }
    case LCLOpcode::RequestSurfaceClose: {
        LOAD_ONE(LCLMsgRequestSurfaceClose, msg);
        out.u32(msg.surfaceId);
        return msg.surfaceId > 0;
    }
    case LCLOpcode::SetInsetBorder: {
        LOAD_ONE(LCLMsgSetInsetBorder, msg);
        out.u32(msg.surfaceId);
        out.u8(msg.enabled);
        return msg.surfaceId > 0 && msg.enabled <= 1;
    }
    case LCLOpcode::SetWindowCornerRadius: {
        LOAD_ONE(LCLMsgSetWindowCornerRadius, msg);
        out.u32(msg.surfaceId);
        out.f32(msg.radius);
        return msg.surfaceId > 0 && validFloat(msg.radius) &&
               msg.radius >= 0.0f;
    }
    case LCLOpcode::SetWindowCornerStyle: {
        LOAD_ONE(LCLMsgSetWindowCornerStyle, msg);
        out.u32(msg.surfaceId);
        out.f32(msg.radius);
        out.f32(msg.roundness);
        return msg.surfaceId > 0 && validFloat(msg.radius) &&
               msg.radius >= 0.0f && validFloat(msg.roundness) &&
               msg.roundness >= 2.0f && msg.roundness <= 8.0f;
    }
    case LCLOpcode::RequestWindowAction: {
        LOAD_ONE(LCLMsgRequestWindowAction, msg);
        out.u32(msg.surfaceId);
        out.u32(static_cast<uint32_t>(msg.action));
        out.f32(msg.localX);
        out.f32(msg.localY);
        return msg.surfaceId > 0 && validAction(msg.action) &&
               validFloat(msg.localX) && validFloat(msg.localY);
    }
    case LCLOpcode::RequestManagedWindowAction: {
        LOAD_ONE(LCLMsgRequestManagedWindowAction, msg);
        out.u32(msg.targetWindowId);
        out.u32(static_cast<uint32_t>(msg.action));
        out.f32(msg.localX);
        out.f32(msg.localY);
        return msg.targetWindowId > 0 && validAction(msg.action) &&
               validFloat(msg.localX) && validFloat(msg.localY);
    }
    case LCLOpcode::SubscribeShellState: {
        LOAD_ONE(LCLMsgSubscribeShellState, msg);
        out.u64(msg.lastKnownRevision);
        return true;
    }
    case LCLOpcode::ShellStateSnapshot: {
        LCLMsgShellStateSnapshot snapshot{};
        if (!loadNative(payload, size, 0, snapshot))
            return false;
        const size_t expected = sizeof(snapshot) +
            static_cast<size_t>(snapshot.sceneCount) * sizeof(LCLMsgShellScene);
        if (snapshot.sceneCount > 4096 || size != expected)
            return false;
        out.u64(snapshot.revision);
        out.u32(snapshot.sceneCount);
        out.u32(snapshot.seatId);
        out.u32(snapshot.displayId);
        out.u32(snapshot.workspaceId);
        out.u64(snapshot.activeSceneId);
        size_t offset = sizeof(snapshot);
        for (uint32_t index = 0; index < snapshot.sceneCount;
             ++index, offset += sizeof(LCLMsgShellScene)) {
            LCLMsgShellScene scene{};
            if (!loadNative(payload, size, offset, scene) || !validShellScene(scene))
                return false;
            encodeShellScene(out, scene);
        }
        return true;
    }
    case LCLOpcode::ShellStateDelta: {
        LOAD_ONE(LCLMsgShellStateDelta, delta);
        if (!validShellDeltaKind(delta.kind))
            return false;
        const bool sceneChange = delta.kind != LCLShellStateDeltaKind::FocusChanged;
        if (sceneChange && !validShellScene(delta.scene))
            return false;
        out.u64(delta.revision);
        out.u32(static_cast<uint32_t>(delta.kind));
        out.u32(delta.seatId);
        out.u32(delta.displayId);
        out.u32(delta.workspaceId);
        out.u64(delta.activeSceneId);
        encodeShellScene(out, delta.scene);
        return true;
    }
    case LCLOpcode::SetSystemSurfaceKind: {
        LOAD_ONE(LCLMsgSetSystemSurfaceKind, msg);
        out.u32(static_cast<uint32_t>(msg.kind));
        return validSystemSurfaceKind(msg.kind) && msg.kind != LCLSystemSurfaceKind::None;
    }
    case LCLOpcode::BeginLaunchPlaceholder: {
        LCLMsgBeginLaunchPlaceholder msg{};
        if (!loadNative(payload, size, 0, msg)) return false;
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        const size_t iconPixelCount =
            static_cast<size_t>(msg.iconWidth) * msg.iconHeight;
        const size_t expectedSize = sizeof(msg) +
            iconPixelCount * sizeof(uint32_t);
        if (msg.homeSurfaceId == 0 || msg.launchToken == 0 ||
            msg.launchToken >= kLaunchTokenLimit ||
            !validString(msg.appId, sizeof(msg.appId)) || msg.appId[0] == '\0' ||
            !validFloat(msg.originX) || !validFloat(msg.originY) ||
            !validFloat(msg.originWidth) || msg.originWidth <= 0.0f ||
            !validFloat(msg.originHeight) || msg.originHeight <= 0.0f ||
            !validFloat(msg.originCornerRadius) || msg.originCornerRadius < 0.0f ||
            msg.iconWidth == 0 || msg.iconHeight == 0 ||
            msg.iconWidth > LCL_LAUNCH_ICON_MAX_DIMENSION ||
            msg.iconHeight > LCL_LAUNCH_ICON_MAX_DIMENSION ||
            size != expectedSize)
            return false;
        out.u32(msg.homeSurfaceId);
        out.u64(msg.launchToken);
        out.fixed(msg.appId, sizeof(msg.appId));
        out.f32(msg.originX);
        out.f32(msg.originY);
        out.f32(msg.originWidth);
        out.f32(msg.originHeight);
        out.f32(msg.originCornerRadius);
        out.u32(msg.iconWidth);
        out.u32(msg.iconHeight);
        size_t offset = sizeof(msg);
        for (size_t index = 0; index < iconPixelCount; ++index) {
            uint32_t pixel = 0;
            if (!loadNative(payload, size, offset, pixel)) return false;
            out.u32(pixel);
            offset += sizeof(pixel);
        }
        return true;
    }
    case LCLOpcode::ResolveLaunchPlaceholder: {
        LOAD_ONE(LCLMsgResolveLaunchPlaceholder, msg);
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (msg.homeSurfaceId == 0 || msg.launchToken == 0 ||
            msg.launchToken >= kLaunchTokenLimit ||
            msg.appInstanceId == 0 || msg.reused > 1) return false;
        out.u32(msg.homeSurfaceId);
        out.u64(msg.launchToken);
        out.u64(msg.appInstanceId);
        out.u8(msg.reused);
        return true;
    }
    case LCLOpcode::CancelLaunchPlaceholder: {
        LOAD_ONE(LCLMsgCancelLaunchPlaceholder, msg);
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (msg.homeSurfaceId == 0 || msg.launchToken == 0 ||
            msg.launchToken >= kLaunchTokenLimit) return false;
        out.u32(msg.homeSurfaceId);
        out.u64(msg.launchToken);
        return true;
    }
    case LCLOpcode::LaunchIconVisibility: {
        LOAD_ONE(LCLMsgLaunchIconVisibility, msg);
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (msg.launchToken == 0 || msg.launchToken >= kLaunchTokenLimit ||
            !validString(msg.appId, sizeof(msg.appId)) ||
            msg.appId[0] == '\0' || msg.visible > 1) return false;
        out.u64(msg.launchToken);
        out.fixed(msg.appId, sizeof(msg.appId));
        out.u8(msg.visible);
        return true;
    }
    case LCLOpcode::LaunchIconVisibilityAck: {
        LOAD_ONE(LCLMsgLaunchIconVisibilityAck, msg);
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (msg.launchToken == 0 || msg.launchToken >= kLaunchTokenLimit ||
            !validString(msg.appId, sizeof(msg.appId)) ||
            msg.appId[0] == '\0') return false;
        out.u64(msg.launchToken);
        out.fixed(msg.appId, sizeof(msg.appId));
        return true;
    }
    case LCLOpcode::SurfaceProducerGrant: {
        LOAD_ONE(LCLMsgSurfaceProducerGrant, msg);
        if (msg.surfaceId == 0 || msg.ownerPid <= 0 ||
            msg.reserved != 0 ||
            (msg.tokenHigh == 0 && msg.tokenLow == 0)) return false;
        out.u32(msg.surfaceId);
        out.u32(static_cast<uint32_t>(msg.ownerPid));
        out.u32(msg.flags);
        out.u32(msg.reserved);
        out.u64(msg.tokenHigh);
        out.u64(msg.tokenLow);
        return true;
    }
    }
    return false;
#undef LOAD_ONE
}

bool decodePayload(LCLOpcode opcode, Reader& in,
                   std::vector<uint8_t>& payload) {
    payload.clear();
    switch (opcode) {
    case LCLOpcode::SurfaceCreate: {
        LCLMsgSurfaceCreate m{};
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (!in.u32(m.surfaceId) || !in.f32(m.x) || !in.f32(m.y) ||
            !in.f32(m.width) || !in.f32(m.height) ||
            !in.fixed(m.title, sizeof(m.title)) ||
            !in.fixed(m.appId, sizeof(m.appId)) ||
            !in.u8(m.hasLaunchOrigin) ||
            !in.f32(m.launchOriginX) || !in.f32(m.launchOriginY) ||
            !in.f32(m.launchOriginWidth) || !in.f32(m.launchOriginHeight) ||
            !in.f32(m.launchOriginCornerRadius) ||
            !in.u64(m.launchToken) || !in.u64(m.appInstanceId) ||
            !in.f32(m.resizeBaseWidth) || !in.f32(m.resizeBaseHeight) ||
            !in.f32(m.resizeWidthIncrement) ||
            !in.f32(m.resizeHeightIncrement))
            return false;
        if (m.surfaceId == 0 || !validFloat(m.x) || !validFloat(m.y) ||
            !validFloat(m.width) || m.width <= 0.0f ||
            !validFloat(m.height) || m.height <= 0.0f ||
            !validString(m.title, sizeof(m.title)) ||
            !validString(m.appId, sizeof(m.appId)) || m.appId[0] == '\0' ||
            m.hasLaunchOrigin > 1 ||
            !validFloat(m.launchOriginX) || !validFloat(m.launchOriginY) ||
            !validFloat(m.launchOriginWidth) || !validFloat(m.launchOriginHeight) ||
            !validFloat(m.launchOriginCornerRadius) ||
            !validResizeConstraint(m.resizeBaseWidth) ||
            !validResizeConstraint(m.resizeBaseHeight) ||
            !validResizeConstraint(m.resizeWidthIncrement) ||
            !validResizeConstraint(m.resizeHeightIncrement) ||
            m.launchToken >= kLaunchTokenLimit ||
            (m.hasLaunchOrigin &&
             (m.launchOriginWidth <= 0.0f || m.launchOriginHeight <= 0.0f ||
              m.launchOriginCornerRadius < 0.0f)))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::PopupSurfaceCreate: {
        LCLMsgPopupSurfaceCreate m{};
        uint32_t role = 0;
        if (!in.u32(m.surfaceId) || !in.u32(m.parentSurfaceId) ||
            !in.u32(role) || !in.f32(m.x) || !in.f32(m.y) ||
            !in.f32(m.width) || !in.f32(m.height))
            return false;
        m.role = static_cast<LCLPopupRole>(role);
        if (m.surfaceId == 0 || m.parentSurfaceId == 0 ||
            m.surfaceId == m.parentSurfaceId || !validFloat(m.x) ||
            !validFloat(m.y) || !validFloat(m.width) || m.width <= 0.0f ||
            !validFloat(m.height) || m.height <= 0.0f || !validPopupRole(m.role))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::AttachedSurfaceCreate: {
        LCLMsgAttachedSurfaceCreate m{};
        uint32_t role = 0;
        if (!in.u32(m.surfaceId) || !in.u32(m.targetWindowId) ||
            !in.u32(role) || !in.f32(m.x) || !in.f32(m.y) ||
            !in.f32(m.width) || !in.f32(m.height) ||
            !in.u8(m.followParentWidth) || !in.u8(m.followParentHeight) ||
            !in.u8(m.acceptsInput))
            return false;
        m.role = static_cast<LCLAttachedSurfaceRole>(role);
        if (m.surfaceId == 0 || m.targetWindowId == 0 ||
            !validAttachedRole(m.role) || !validFloat(m.x) ||
            !validFloat(m.y) || !validFloat(m.width) || m.width <= 0.0f ||
            !validFloat(m.height) || m.height <= 0.0f ||
            m.followParentWidth > 1 || m.followParentHeight > 1 ||
            m.acceptsInput > 1)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SurfaceDestroy: {
        LCLMsgSurfaceDestroy m{};
        if (!in.u32(m.surfaceId) || m.surfaceId == 0)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::ConfigureBounds: {
        LCLMsgConfigureBounds m{};
        uint8_t resizeReason = 0;
        if (!in.u32(m.surfaceId) || !in.u64(m.configureSerial) ||
            !in.u64(m.geometryGeneration) ||
            !in.f32(m.x) || !in.f32(m.y) ||
            !in.f32(m.width) || !in.f32(m.height) ||
            !in.f32(m.backingWidth) || !in.f32(m.backingHeight) ||
            !in.u8(m.isFocused) || !in.fixed(m.title, sizeof(m.title)) ||
            !in.f32(m.bufferScale) || !in.u8(resizeReason))
            return false;
        m.resizeReason = static_cast<LCLConfigureResizeReason>(resizeReason);
        if (m.surfaceId == 0 || m.configureSerial == 0 ||
            !validFloat(m.x) || !validFloat(m.y) ||
            !validFloat(m.width) || m.width <= 0.0f ||
            !validFloat(m.height) || m.height <= 0.0f ||
            !validFloat(m.backingWidth) || !validFloat(m.backingHeight) ||
            m.backingWidth < m.width || m.backingHeight < m.height || m.isFocused > 1 ||
            !validScale(m.bufferScale) || !validConfigureResizeReason(m.resizeReason) ||
            !validString(m.title, sizeof(m.title)))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::FramePresented: {
        LCLMsgFramePresented m{};
        if (!in.u32(m.surfaceId) || !in.u64(m.configureSerial) ||
            !in.u64(m.frameSerial) || !in.u64(m.geometryGeneration) ||
            !in.u64(m.displaySequence) ||
            !in.u64(m.timestampNs) || !in.u64(m.refreshIntervalNs) ||
            m.surfaceId == 0 || m.configureSerial == 0 || m.frameSerial == 0 ||
            m.displaySequence == 0 || m.timestampNs == 0 ||
            m.refreshIntervalNs == 0)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::FrameDiscarded: {
        LCLMsgFrameDiscarded m{};
        uint32_t reason = 0;
        if (!in.u32(m.surfaceId) || !in.u64(m.configureSerial) ||
            !in.u64(m.frameSerial) || !in.u64(m.geometryGeneration) ||
            !in.u32(reason) || m.surfaceId == 0 ||
            m.configureSerial == 0 || m.frameSerial == 0)
            return false;
        m.reason = static_cast<LCLFrameDiscardReason>(reason);
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::AckResponse: {
        LCLMsgAckResponse m{};
        if (!in.u32(m.status) || !in.fixed(m.message, sizeof(m.message)) ||
            !validString(m.message, sizeof(m.message)))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::InputEvent: {
        LCLMsgInputEvent m{};
        if (!in.u32(m.surfaceId) || !in.u32(m.type) || !in.u32(m.key) ||
            !in.u8(m.pressed) || !in.u8(m.modifiers) || !in.u8(m.source) ||
            !in.u32(m.pointerId) || !in.u32(m.codepoint) ||
            !in.f32(m.x) || !in.f32(m.y) || !in.f32(m.deltaX) || !in.f32(m.deltaY))
            return false;
        if (m.surfaceId == 0 ||
            m.type < static_cast<uint32_t>(LCLInputEventType::KeyDown) ||
            m.type > static_cast<uint32_t>(LCLInputEventType::PointerCancel) ||
            m.pressed > 1 ||
            !validFloat(m.x) || !validFloat(m.y) || !validFloat(m.deltaX) || !validFloat(m.deltaY))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetDecorationMode: {
        LCLMsgSetDecorationMode m{};
        uint32_t mode = 0;
        if (!in.u32(m.surfaceId) || !in.u32(mode))
            return false;
        m.mode = static_cast<LCLDecorationMode>(mode);
        if (m.surfaceId == 0 || !validDecoration(m.mode))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetEdgeToEdge: {
        LCLMsgSetEdgeToEdge m{};
        if (!in.u32(m.surfaceId) || !in.u8(m.enabled))
            return false;
        if (m.surfaceId == 0 || m.enabled > 1)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetWindowLayer: {
        LCLMsgSetWindowLayer m{};
        uint32_t layer = 0;
        if (!in.u32(m.surfaceId) || !in.u32(layer) || !in.u8(m.unfocusable))
            return false;
        m.layer = static_cast<LCLWindowLayer>(layer);
        if (m.surfaceId == 0 || !validLayer(m.layer) || m.unfocusable > 1)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetReservedZone: {
        LCLMsgSetReservedZone m{};
        if (!in.u32(m.surfaceId) || !in.f32(m.top) || !in.f32(m.bottom) ||
            !in.f32(m.left) || !in.f32(m.right) || m.surfaceId == 0 ||
            !validFloat(m.top) || m.top < 0.0f ||
            !validFloat(m.bottom) || m.bottom < 0.0f ||
            !validFloat(m.left) || m.left < 0.0f ||
            !validFloat(m.right) || m.right < 0.0f)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetEffectGraph: {
        LCLMsgSetEffectGraphHeader graph{};
        if (!in.u32(graph.surfaceId) || !in.u32(graph.regionCount) ||
            !in.u32(graph.filterCount))
            return false;
        if (graph.surfaceId == 0 || graph.regionCount > kMaxEffectRegions ||
            graph.filterCount > kMaxEffectFilters)
            return false;
        appendNative(payload, graph);
        for (uint32_t i = 0; i < graph.regionCount; ++i) {
            EffectRegion r{};
            if (!decodeRegion(in, r) || r.filterOffset > graph.filterCount ||
                r.filterCount > graph.filterCount - r.filterOffset)
                return false;
            appendNative(payload, r);
        }
        for (uint32_t i = 0; i < graph.filterCount; ++i) {
            FilterOp op{};
            if (!decodeFilter(in, op))
                return false;
            appendNative(payload, op);
        }
        break;
    }
    case LCLOpcode::ClearEffectGraph: {
        LCLMsgClearEffectGraph m{};
        if (!in.u32(m.surfaceId) || m.surfaceId == 0)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::BeginWindowMove: {
        LCLMsgBeginWindowMove m{};
        if (!in.u32(m.surfaceId) || !in.f32(m.localX) || !in.f32(m.localY) ||
            m.surfaceId == 0 || !validFloat(m.localX) || !validFloat(m.localY))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::RequestSurfaceClose: {
        LCLMsgRequestSurfaceClose m{};
        if (!in.u32(m.surfaceId) || m.surfaceId == 0)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetInsetBorder: {
        LCLMsgSetInsetBorder m{};
        if (!in.u32(m.surfaceId) || !in.u8(m.enabled) || m.surfaceId == 0 ||
            m.enabled > 1)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetWindowCornerRadius: {
        LCLMsgSetWindowCornerRadius m{};
        if (!in.u32(m.surfaceId) || !in.f32(m.radius) || m.surfaceId == 0 ||
            !validFloat(m.radius) || m.radius < 0.0f)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SetWindowCornerStyle: {
        LCLMsgSetWindowCornerStyle m{};
        if (!in.u32(m.surfaceId) || !in.f32(m.radius) || !in.f32(m.roundness) ||
            m.surfaceId == 0 || !validFloat(m.radius) || m.radius < 0.0f ||
            !validFloat(m.roundness) || m.roundness < 2.0f || m.roundness > 8.0f)
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::RequestWindowAction: {
        LCLMsgRequestWindowAction m{};
        uint32_t action = 0;
        if (!in.u32(m.surfaceId) || !in.u32(action) || !in.f32(m.localX) ||
            !in.f32(m.localY))
            return false;
        m.action = static_cast<LCLWindowAction>(action);
        if (m.surfaceId == 0 || !validAction(m.action) || !validFloat(m.localX) ||
            !validFloat(m.localY))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::RequestManagedWindowAction: {
        LCLMsgRequestManagedWindowAction m{};
        uint32_t action = 0;
        if (!in.u32(m.targetWindowId) || !in.u32(action) ||
            !in.f32(m.localX) || !in.f32(m.localY))
            return false;
        m.action = static_cast<LCLWindowAction>(action);
        if (m.targetWindowId == 0 || !validAction(m.action) ||
            !validFloat(m.localX) || !validFloat(m.localY))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SubscribeShellState: {
        LCLMsgSubscribeShellState m{};
        if (!in.u64(m.lastKnownRevision))
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::ShellStateSnapshot: {
        LCLMsgShellStateSnapshot snapshot{};
        if (!in.u64(snapshot.revision) || !in.u32(snapshot.sceneCount) ||
            !in.u32(snapshot.seatId) || !in.u32(snapshot.displayId) ||
            !in.u32(snapshot.workspaceId) || !in.u64(snapshot.activeSceneId) ||
            snapshot.sceneCount > 4096)
            return false;
        appendNative(payload, snapshot);
        for (uint32_t index = 0; index < snapshot.sceneCount; ++index) {
            LCLMsgShellScene scene{};
            if (!decodeShellScene(in, scene))
                return false;
            appendNative(payload, scene);
        }
        break;
    }
    case LCLOpcode::ShellStateDelta: {
        LCLMsgShellStateDelta delta{};
        uint32_t kind = 0;
        if (!in.u64(delta.revision) || !in.u32(kind) || !in.u32(delta.seatId) ||
            !in.u32(delta.displayId) || !in.u32(delta.workspaceId) ||
            !in.u64(delta.activeSceneId))
            return false;
        delta.kind = static_cast<LCLShellStateDeltaKind>(kind);
        if (!validShellDeltaKind(delta.kind) ||
            !decodeShellScene(in, delta.scene,
                delta.kind != LCLShellStateDeltaKind::FocusChanged))
            return false;
        appendNative(payload, delta);
        break;
    }
    case LCLOpcode::SetSystemSurfaceKind: {
        LCLMsgSetSystemSurfaceKind msg{};
        uint32_t kind = 0;
        if (!in.u32(kind)) return false;
        msg.kind = static_cast<LCLSystemSurfaceKind>(kind);
        if (!validSystemSurfaceKind(msg.kind) || msg.kind == LCLSystemSurfaceKind::None)
            return false;
        appendNative(payload, msg);
        break;
    }
    case LCLOpcode::BeginLaunchPlaceholder: {
        LCLMsgBeginLaunchPlaceholder m{};
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (!in.u32(m.homeSurfaceId) || !in.u64(m.launchToken) ||
            !in.fixed(m.appId, sizeof(m.appId)) ||
            !in.f32(m.originX) || !in.f32(m.originY) ||
            !in.f32(m.originWidth) || !in.f32(m.originHeight) ||
            !in.f32(m.originCornerRadius) ||
            !in.u32(m.iconWidth) || !in.u32(m.iconHeight) ||
            m.homeSurfaceId == 0 || m.launchToken == 0 ||
            m.launchToken >= kLaunchTokenLimit ||
            !validString(m.appId, sizeof(m.appId)) || m.appId[0] == '\0' ||
            !validFloat(m.originX) || !validFloat(m.originY) ||
            !validFloat(m.originWidth) || m.originWidth <= 0.0f ||
            !validFloat(m.originHeight) || m.originHeight <= 0.0f ||
            !validFloat(m.originCornerRadius) || m.originCornerRadius < 0.0f ||
            m.iconWidth == 0 || m.iconHeight == 0 ||
            m.iconWidth > LCL_LAUNCH_ICON_MAX_DIMENSION ||
            m.iconHeight > LCL_LAUNCH_ICON_MAX_DIMENSION)
            return false;
        appendNative(payload, m);
        const size_t iconPixelCount =
            static_cast<size_t>(m.iconWidth) * m.iconHeight;
        for (size_t index = 0; index < iconPixelCount; ++index) {
            uint32_t pixel = 0;
            if (!in.u32(pixel)) return false;
            appendNative(payload, pixel);
        }
        break;
    }
    case LCLOpcode::ResolveLaunchPlaceholder: {
        LCLMsgResolveLaunchPlaceholder m{};
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (!in.u32(m.homeSurfaceId) || !in.u64(m.launchToken) ||
            !in.u64(m.appInstanceId) || !in.u8(m.reused) ||
            m.homeSurfaceId == 0 || m.launchToken == 0 ||
            m.launchToken >= kLaunchTokenLimit ||
            m.appInstanceId == 0 || m.reused > 1) return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::CancelLaunchPlaceholder: {
        LCLMsgCancelLaunchPlaceholder m{};
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (!in.u32(m.homeSurfaceId) || !in.u64(m.launchToken) ||
            m.homeSurfaceId == 0 || m.launchToken == 0 ||
            m.launchToken >= kLaunchTokenLimit) return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::LaunchIconVisibility: {
        LCLMsgLaunchIconVisibility m{};
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (!in.u64(m.launchToken) ||
            !in.fixed(m.appId, sizeof(m.appId)) || !in.u8(m.visible) ||
            m.launchToken == 0 || m.launchToken >= kLaunchTokenLimit ||
            !validString(m.appId, sizeof(m.appId)) ||
            m.appId[0] == '\0' || m.visible > 1) return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::LaunchIconVisibilityAck: {
        LCLMsgLaunchIconVisibilityAck m{};
        constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
        if (!in.u64(m.launchToken) || !in.fixed(m.appId, sizeof(m.appId)) ||
            m.launchToken == 0 || m.launchToken >= kLaunchTokenLimit ||
            !validString(m.appId, sizeof(m.appId)) || m.appId[0] == '\0')
            return false;
        appendNative(payload, m);
        break;
    }
    case LCLOpcode::SurfaceProducerGrant: {
        LCLMsgSurfaceProducerGrant m{};
        uint32_t ownerPid = 0;
        if (!in.u32(m.surfaceId) || !in.u32(ownerPid) || !in.u32(m.flags) ||
            !in.u32(m.reserved) ||
            !in.u64(m.tokenHigh) || !in.u64(m.tokenLow)) return false;
        m.ownerPid = static_cast<int32_t>(ownerPid);
        if (m.surfaceId == 0 || m.ownerPid <= 0 || m.reserved != 0 ||
            (m.tokenHigh == 0 && m.tokenLow == 0)) return false;
        appendNative(payload, m);
        break;
    }
    }
    return in.done();
}

struct PendingPacket {
    std::vector<uint8_t> bytes;
    int fd{-1};
};
struct PendingQueue {
    std::deque<PendingPacket> packets;
    size_t bytes{0};
};
std::mutex g_queueMutex;
std::unordered_map<int, PendingQueue> g_queues;

bool sendPacketNow(int socketFd, const std::vector<uint8_t>& packet,
                   int passedFd) {
    iovec iov{const_cast<uint8_t*>(packet.data()), packet.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    std::array<char, CMSG_SPACE(sizeof(int))> control{};
    if (passedFd >= 0) {
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &passedFd, sizeof(int));
    }
    const ssize_t sent = sendmsg(socketFd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(packet.size()))
        return true;
    if (sent >= 0)
        errno = EPROTO;
    return false;
}

bool hasPendingPackets(int socketFd) {
    std::lock_guard lock(g_queueMutex);
    auto it = g_queues.find(socketFd);
    return it != g_queues.end() && !it->second.packets.empty();
}

bool queuePacket(int socketFd, std::vector<uint8_t> packet, int passedFd) {
    int ownedFd = -1;
    if (passedFd >= 0) {
        ownedFd = fcntl(passedFd, F_DUPFD_CLOEXEC, 0);
        if (ownedFd < 0)
            return false;
    }

    std::lock_guard lock(g_queueMutex);
    auto& queue = g_queues[socketFd];
    if (queue.bytes + packet.size() > kMaxQueuedBytesPerSocket) {
        if (ownedFd >= 0)
            close(ownedFd);
        errno = ENOBUFS;
        return false;
    }
    queue.bytes += packet.size();
    queue.packets.push_back({std::move(packet), ownedFd});
    return true;
}

} // namespace

bool encodePacket(const LCLHeader& header, const void* nativePayload,
                  std::vector<uint8_t>& packet) {
    if (header.magic != LCL_PROTOCOL_MAGIC ||
        header.version != LCL_PROTOCOL_VERSION || header.flags != 0 ||
        !validOpcode(header.opcode) ||
        header.payloadSize > LCL_PROTOCOL_MAX_PAYLOAD ||
        (header.payloadSize > 0 && !nativePayload))
        return false;
    Writer payloadWriter;
    if (!encodePayload(header.opcode, nativePayload, header.payloadSize,
                       payloadWriter))
        return false;
    auto wirePayload = payloadWriter.take();
    if (wirePayload.size() != header.payloadSize)
        return false;
    Writer out;
    out.u32(header.magic);
    out.u32(header.version);
    out.u32(static_cast<uint32_t>(header.opcode));
    out.u32(header.flags);
    out.u32(header.requestId);
    out.u32(static_cast<uint32_t>(wirePayload.size()));
    packet = out.take();
    packet.insert(packet.end(), wirePayload.begin(), wirePayload.end());
    return true;
}

bool decodePacket(const uint8_t* packet, size_t packetSize, LCLHeader& header,
                  std::vector<uint8_t>& nativePayload) {
    nativePayload.clear();
    if (!packet || packetSize < LCL_PROTOCOL_WIRE_HEADER_SIZE)
        return false;
    Reader head(packet, LCL_PROTOCOL_WIRE_HEADER_SIZE);
    uint32_t opcode = 0;
    if (!head.u32(header.magic) || !head.u32(header.version) ||
        !head.u32(opcode) || !head.u32(header.flags) ||
        !head.u32(header.requestId) || !head.u32(header.payloadSize) ||
        !head.done())
        return false;
    header.opcode = static_cast<LCLOpcode>(opcode);
    if (header.magic != LCL_PROTOCOL_MAGIC ||
        header.version != LCL_PROTOCOL_VERSION || header.flags != 0 ||
        !validOpcode(header.opcode) ||
        header.payloadSize > LCL_PROTOCOL_MAX_PAYLOAD ||
        packetSize != LCL_PROTOCOL_WIRE_HEADER_SIZE + header.payloadSize)
        return false;
    Reader payloadReader(packet + LCL_PROTOCOL_WIRE_HEADER_SIZE,
                         header.payloadSize);
    return decodePayload(header.opcode, payloadReader, nativePayload) &&
           nativePayload.size() == header.payloadSize;
}

bool flushPendingWrites(int socketFd) {
    std::lock_guard lock(g_queueMutex);
    auto it = g_queues.find(socketFd);
    if (it == g_queues.end())
        return true;
    auto& queue = it->second;
    while (!queue.packets.empty()) {
        auto& packet = queue.packets.front();
        if (!sendPacketNow(socketFd, packet.bytes, packet.fd)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            return false;
        }
        if (packet.fd >= 0)
            close(packet.fd);
        queue.bytes -= packet.bytes.size();
        queue.packets.pop_front();
    }
    g_queues.erase(it);
    return true;
}

void discardPendingWrites(int socketFd) {
    std::lock_guard lock(g_queueMutex);
    auto it = g_queues.find(socketFd);
    if (it == g_queues.end())
        return;
    for (auto& packet : it->second.packets) {
        if (packet.fd >= 0)
            close(packet.fd);
    }
    g_queues.erase(it);
}

bool sendMsgWithFd(int socketFd, const LCLHeader& header, const void* payload,
                   int passedFd) {
    if (socketFd < 0)
        return false;
    if (passedFd >= 0 || passedFd < -1) {
        errno = EPROTO;
        return false;
    }
    std::vector<uint8_t> packet;
    if (!encodePacket(header, payload, packet)) {
        errno = EPROTO;
        return false;
    }
    if (!flushPendingWrites(socketFd))
        return false;
    // A prior packet may still be queued after EAGAIN. Preserve request order
    // instead of allowing a newer packet to overtake it on the socket.
    if (hasPendingPackets(socketFd))
        return queuePacket(socketFd, std::move(packet), passedFd);
    if (sendPacketNow(socketFd, packet, passedFd))
        return true;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        return false;
    return queuePacket(socketFd, std::move(packet), passedFd);
}

ReceiveStatus recvPacketWithFd(int socketFd, LCLHeader& header,
                               std::vector<uint8_t>& payload, int& receivedFd) {
    receivedFd = -1;
    if (socketFd < 0)
        return ReceiveStatus::IoError;
    if (!flushPendingWrites(socketFd))
        return ReceiveStatus::IoError;
    thread_local std::vector<uint8_t> packet(LCL_PROTOCOL_WIRE_HEADER_SIZE +
                                             LCL_PROTOCOL_MAX_PAYLOAD);
    iovec iov{packet.data(), packet.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    std::array<char, CMSG_SPACE(sizeof(int) * 4)> control{};
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    const ssize_t count =
        recvmsg(socketFd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count == 0)
        return ReceiveStatus::Closed;
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return ReceiveStatus::WouldBlock;
        return ReceiveStatus::IoError;
    }
    std::vector<int> descriptors;
    bool invalidAncillaryData = false;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(0)) {
            invalidAncillaryData = true;
            continue;
        }
        const size_t bytes = c->cmsg_len - CMSG_LEN(0);
        if (bytes == 0 || bytes % sizeof(int) != 0) {
            invalidAncillaryData = true;
            continue;
        }
        const size_t n = bytes / sizeof(int);
        const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
        descriptors.insert(descriptors.end(), fds, fds + n);
    }
    auto reject = [&]() {
        for (int fd : descriptors)
            close(fd);
        errno = EPROTO;
        return ReceiveStatus::Invalid;
    };
    if (invalidAncillaryData ||
        (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        descriptors.size() > 1)
        return reject();
    if (!decodePacket(packet.data(), static_cast<size_t>(count), header, payload))
        return reject();
    if (!descriptors.empty()) return reject();
    return ReceiveStatus::Received;
}

bool recvMsgWithFd(int socketFd, LCLHeader& header,
                   std::vector<uint8_t>& payload, int& receivedFd) {
    const auto status = recvPacketWithFd(socketFd, header, payload, receivedFd);
    if (status == ReceiveStatus::Received)
        return true;
    if (status == ReceiveStatus::WouldBlock)
        errno = EAGAIN;
    else if (status == ReceiveStatus::Closed)
        errno = ECONNRESET;
    else if (status == ReceiveStatus::Invalid)
        errno = EPROTO;
    return false;
}

} // namespace lcl::protocol
