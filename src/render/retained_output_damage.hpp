#pragma once

#include "render/raster_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace lcl::render {

// Tracks the scene revision stored in each rotating producer buffer. A reused
// buffer may have missed one or more retained-scene patches while the
// compositor owned it; only the union of those patches must be copied from the
// authoritative scene texture before the buffer is exported again.
class RetainedOutputDamageTracker {
public:
    struct Frame {
        uint64_t frameSerial{0};
        uint64_t baseFrameSerial{0};
        uint64_t geometryGeneration{0};
        uint32_t width{0};
        uint32_t height{0};
        float scale{1.0f};
        RasterRect damage{};
        bool replacesScene{false};
    };

    // nullopt means that the complete retained scene must be copied.
    std::optional<RasterRect> copyDamage(uint32_t bufferId,
                                         const Frame& frame) const {
        if (bufferId == 0 || frame.replacesScene) return std::nullopt;

        const auto stored = m_buffers.find(bufferId);
        if (stored == m_buffers.end() ||
            !sameGeometry(stored->second, frame)) {
            return std::nullopt;
        }

        uint64_t cursor = stored->second.frameSerial;
        RasterRect merged{};
        bool hasDamage = false;
        for (const auto& patch : m_history) {
            if (cursor == frame.baseFrameSerial) break;
            if (patch.baseFrameSerial != cursor ||
                !sameGeometry(patch, frame)) {
                continue;
            }
            mergeDamage(merged, hasDamage, patch.damage);
            cursor = patch.frameSerial;
        }
        if (cursor != frame.baseFrameSerial) return std::nullopt;

        mergeDamage(merged, hasDamage, frame.damage);
        return hasDamage ? std::optional<RasterRect>(merged) : std::nullopt;
    }

    void commit(uint32_t bufferId, const Frame& frame) {
        if (bufferId == 0) return;
        if (frame.replacesScene) {
            m_history.clear();
            m_buffers.clear();
        } else {
            m_history.push_back(frame);
            while (m_history.size() > kMaxHistory) m_history.pop_front();
        }
        m_buffers[bufferId] = frame;
    }

    void invalidateBuffer(uint32_t bufferId) {
        m_buffers.erase(bufferId);
    }

    void reset() {
        m_buffers.clear();
        m_history.clear();
    }

private:
    static constexpr size_t kMaxHistory = 64;

    static bool sameGeometry(const Frame& left, const Frame& right) {
        return left.geometryGeneration == right.geometryGeneration &&
            left.width == right.width && left.height == right.height &&
            std::fabs(left.scale - right.scale) <= 0.0001f;
    }

    static void mergeDamage(RasterRect& merged, bool& hasDamage,
                            const RasterRect& damage) {
        if (damage.width <= 0.0f || damage.height <= 0.0f) return;
        if (!hasDamage) {
            merged = damage;
            hasDamage = true;
            return;
        }
        const float left = std::min(merged.x, damage.x);
        const float top = std::min(merged.y, damage.y);
        const float right = std::max(
            merged.x + merged.width, damage.x + damage.width);
        const float bottom = std::max(
            merged.y + merged.height, damage.y + damage.height);
        merged = {left, top, right - left, bottom - top};
    }

    std::unordered_map<uint32_t, Frame> m_buffers;
    std::deque<Frame> m_history;
};

} // namespace lcl::render
