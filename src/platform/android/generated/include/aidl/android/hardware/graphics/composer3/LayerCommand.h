/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/LayerCommand.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <android/binder_parcelable_utils.h>
#include <android/binder_to_string.h>
#include <aidl/android/hardware/common/NativeHandle.h>
#include <aidl/android/hardware/graphics/common/FRect.h>
#include <aidl/android/hardware/graphics/common/Point.h>
#include <aidl/android/hardware/graphics/common/Rect.h>
#include <aidl/android/hardware/graphics/composer3/Buffer.h>
#include <aidl/android/hardware/graphics/composer3/Color.h>
#include <aidl/android/hardware/graphics/composer3/LayerBrightness.h>
#include <aidl/android/hardware/graphics/composer3/LayerLifecycleBatchCommandType.h>
#include <aidl/android/hardware/graphics/composer3/Luts.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableBlendMode.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableComposition.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableDataspace.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableTransform.h>
#include <aidl/android/hardware/graphics/composer3/PerFrameMetadata.h>
#include <aidl/android/hardware/graphics/composer3/PerFrameMetadataBlob.h>
#include <aidl/android/hardware/graphics/composer3/PlaneAlpha.h>
#include <aidl/android/hardware/graphics/composer3/ZOrder.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::common {
class NativeHandle;
}  // namespace aidl::android::hardware::common
namespace aidl::android::hardware::graphics::common {
class FRect;
class Point;
class Rect;
}  // namespace aidl::android::hardware::graphics::common
namespace aidl::android::hardware::graphics::composer3 {
class Buffer;
class Color;
class LayerBrightness;
class Luts;
class ParcelableBlendMode;
class ParcelableComposition;
class ParcelableDataspace;
class ParcelableTransform;
class PerFrameMetadata;
class PerFrameMetadataBlob;
class PlaneAlpha;
class ZOrder;
}  // namespace aidl::android::hardware::graphics::composer3
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class LayerCommand {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  int64_t layer = 0L;
  std::optional<::aidl::android::hardware::graphics::common::Point> cursorPosition;
  std::optional<::aidl::android::hardware::graphics::composer3::Buffer> buffer;
  std::optional<std::vector<std::optional<::aidl::android::hardware::graphics::common::Rect>>> damage;
  std::optional<::aidl::android::hardware::graphics::composer3::ParcelableBlendMode> blendMode;
  std::optional<::aidl::android::hardware::graphics::composer3::Color> color;
  std::optional<::aidl::android::hardware::graphics::composer3::ParcelableComposition> composition;
  std::optional<::aidl::android::hardware::graphics::composer3::ParcelableDataspace> dataspace;
  std::optional<::aidl::android::hardware::graphics::common::Rect> displayFrame;
  std::optional<::aidl::android::hardware::graphics::composer3::PlaneAlpha> planeAlpha;
  std::optional<::aidl::android::hardware::common::NativeHandle> sidebandStream;
  std::optional<::aidl::android::hardware::graphics::common::FRect> sourceCrop;
  std::optional<::aidl::android::hardware::graphics::composer3::ParcelableTransform> transform;
  std::optional<std::vector<std::optional<::aidl::android::hardware::graphics::common::Rect>>> visibleRegion;
  std::optional<::aidl::android::hardware::graphics::composer3::ZOrder> z;
  std::optional<std::vector<float>> colorTransform;
  std::optional<::aidl::android::hardware::graphics::composer3::LayerBrightness> brightness;
  std::optional<std::vector<std::optional<::aidl::android::hardware::graphics::composer3::PerFrameMetadata>>> perFrameMetadata;
  std::optional<std::vector<std::optional<::aidl::android::hardware::graphics::composer3::PerFrameMetadataBlob>>> perFrameMetadataBlob;
  std::optional<std::vector<std::optional<::aidl::android::hardware::graphics::common::Rect>>> blockingRegion;
  std::optional<std::vector<int32_t>> bufferSlotsToClear;
  ::aidl::android::hardware::graphics::composer3::LayerLifecycleBatchCommandType layerLifecycleBatchCommandType = ::aidl::android::hardware::graphics::composer3::LayerLifecycleBatchCommandType(0);
  int32_t newBufferSlotCount = 0;
  std::optional<::aidl::android::hardware::graphics::composer3::Luts> luts;
  int64_t pictureProfileId = 0L;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const LayerCommand& _rhs) const {
    return std::tie(layer, cursorPosition, buffer, damage, blendMode, color, composition, dataspace, displayFrame, planeAlpha, sidebandStream, sourceCrop, transform, visibleRegion, z, colorTransform, brightness, perFrameMetadata, perFrameMetadataBlob, blockingRegion, bufferSlotsToClear, layerLifecycleBatchCommandType, newBufferSlotCount, luts, pictureProfileId) == std::tie(_rhs.layer, _rhs.cursorPosition, _rhs.buffer, _rhs.damage, _rhs.blendMode, _rhs.color, _rhs.composition, _rhs.dataspace, _rhs.displayFrame, _rhs.planeAlpha, _rhs.sidebandStream, _rhs.sourceCrop, _rhs.transform, _rhs.visibleRegion, _rhs.z, _rhs.colorTransform, _rhs.brightness, _rhs.perFrameMetadata, _rhs.perFrameMetadataBlob, _rhs.blockingRegion, _rhs.bufferSlotsToClear, _rhs.layerLifecycleBatchCommandType, _rhs.newBufferSlotCount, _rhs.luts, _rhs.pictureProfileId);
  }
  inline bool operator<(const LayerCommand& _rhs) const {
    return std::tie(layer, cursorPosition, buffer, damage, blendMode, color, composition, dataspace, displayFrame, planeAlpha, sidebandStream, sourceCrop, transform, visibleRegion, z, colorTransform, brightness, perFrameMetadata, perFrameMetadataBlob, blockingRegion, bufferSlotsToClear, layerLifecycleBatchCommandType, newBufferSlotCount, luts, pictureProfileId) < std::tie(_rhs.layer, _rhs.cursorPosition, _rhs.buffer, _rhs.damage, _rhs.blendMode, _rhs.color, _rhs.composition, _rhs.dataspace, _rhs.displayFrame, _rhs.planeAlpha, _rhs.sidebandStream, _rhs.sourceCrop, _rhs.transform, _rhs.visibleRegion, _rhs.z, _rhs.colorTransform, _rhs.brightness, _rhs.perFrameMetadata, _rhs.perFrameMetadataBlob, _rhs.blockingRegion, _rhs.bufferSlotsToClear, _rhs.layerLifecycleBatchCommandType, _rhs.newBufferSlotCount, _rhs.luts, _rhs.pictureProfileId);
  }
  inline bool operator!=(const LayerCommand& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const LayerCommand& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const LayerCommand& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const LayerCommand& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "LayerCommand{";
    _aidl_os << "layer: " << ::android::internal::ToString(layer);
    _aidl_os << ", cursorPosition: " << ::android::internal::ToString(cursorPosition);
    _aidl_os << ", buffer: " << ::android::internal::ToString(buffer);
    _aidl_os << ", damage: " << ::android::internal::ToString(damage);
    _aidl_os << ", blendMode: " << ::android::internal::ToString(blendMode);
    _aidl_os << ", color: " << ::android::internal::ToString(color);
    _aidl_os << ", composition: " << ::android::internal::ToString(composition);
    _aidl_os << ", dataspace: " << ::android::internal::ToString(dataspace);
    _aidl_os << ", displayFrame: " << ::android::internal::ToString(displayFrame);
    _aidl_os << ", planeAlpha: " << ::android::internal::ToString(planeAlpha);
    _aidl_os << ", sidebandStream: " << ::android::internal::ToString(sidebandStream);
    _aidl_os << ", sourceCrop: " << ::android::internal::ToString(sourceCrop);
    _aidl_os << ", transform: " << ::android::internal::ToString(transform);
    _aidl_os << ", visibleRegion: " << ::android::internal::ToString(visibleRegion);
    _aidl_os << ", z: " << ::android::internal::ToString(z);
    _aidl_os << ", colorTransform: " << ::android::internal::ToString(colorTransform);
    _aidl_os << ", brightness: " << ::android::internal::ToString(brightness);
    _aidl_os << ", perFrameMetadata: " << ::android::internal::ToString(perFrameMetadata);
    _aidl_os << ", perFrameMetadataBlob: " << ::android::internal::ToString(perFrameMetadataBlob);
    _aidl_os << ", blockingRegion: " << ::android::internal::ToString(blockingRegion);
    _aidl_os << ", bufferSlotsToClear: " << ::android::internal::ToString(bufferSlotsToClear);
    _aidl_os << ", layerLifecycleBatchCommandType: " << ::android::internal::ToString(layerLifecycleBatchCommandType);
    _aidl_os << ", newBufferSlotCount: " << ::android::internal::ToString(newBufferSlotCount);
    _aidl_os << ", luts: " << ::android::internal::ToString(luts);
    _aidl_os << ", pictureProfileId: " << ::android::internal::ToString(pictureProfileId);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
