/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/DisplayCommand.aidl
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
#include <aidl/android/hardware/graphics/composer3/Buffer.h>
#include <aidl/android/hardware/graphics/composer3/ClientTarget.h>
#include <aidl/android/hardware/graphics/composer3/ClockMonotonicTimestamp.h>
#include <aidl/android/hardware/graphics/composer3/DisplayBrightness.h>
#include <aidl/android/hardware/graphics/composer3/LayerCommand.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::graphics::composer3 {
class Buffer;
class ClientTarget;
class ClockMonotonicTimestamp;
class DisplayBrightness;
class LayerCommand;
}  // namespace aidl::android::hardware::graphics::composer3
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class DisplayCommand {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  int64_t display = 0L;
  std::vector<::aidl::android::hardware::graphics::composer3::LayerCommand> layers;
  std::optional<std::vector<float>> colorTransformMatrix;
  std::optional<::aidl::android::hardware::graphics::composer3::DisplayBrightness> brightness;
  std::optional<::aidl::android::hardware::graphics::composer3::ClientTarget> clientTarget;
  std::optional<::aidl::android::hardware::graphics::composer3::Buffer> virtualDisplayOutputBuffer;
  std::optional<::aidl::android::hardware::graphics::composer3::ClockMonotonicTimestamp> expectedPresentTime;
  bool validateDisplay = false;
  bool acceptDisplayChanges = false;
  bool presentDisplay = false;
  bool presentOrValidateDisplay = false;
  int32_t frameIntervalNs = 0;
  int64_t pictureProfileId = 0L;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const DisplayCommand& _rhs) const {
    return std::tie(display, layers, colorTransformMatrix, brightness, clientTarget, virtualDisplayOutputBuffer, expectedPresentTime, validateDisplay, acceptDisplayChanges, presentDisplay, presentOrValidateDisplay, frameIntervalNs, pictureProfileId) == std::tie(_rhs.display, _rhs.layers, _rhs.colorTransformMatrix, _rhs.brightness, _rhs.clientTarget, _rhs.virtualDisplayOutputBuffer, _rhs.expectedPresentTime, _rhs.validateDisplay, _rhs.acceptDisplayChanges, _rhs.presentDisplay, _rhs.presentOrValidateDisplay, _rhs.frameIntervalNs, _rhs.pictureProfileId);
  }
  inline bool operator<(const DisplayCommand& _rhs) const {
    return std::tie(display, layers, colorTransformMatrix, brightness, clientTarget, virtualDisplayOutputBuffer, expectedPresentTime, validateDisplay, acceptDisplayChanges, presentDisplay, presentOrValidateDisplay, frameIntervalNs, pictureProfileId) < std::tie(_rhs.display, _rhs.layers, _rhs.colorTransformMatrix, _rhs.brightness, _rhs.clientTarget, _rhs.virtualDisplayOutputBuffer, _rhs.expectedPresentTime, _rhs.validateDisplay, _rhs.acceptDisplayChanges, _rhs.presentDisplay, _rhs.presentOrValidateDisplay, _rhs.frameIntervalNs, _rhs.pictureProfileId);
  }
  inline bool operator!=(const DisplayCommand& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const DisplayCommand& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const DisplayCommand& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const DisplayCommand& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "DisplayCommand{";
    _aidl_os << "display: " << ::android::internal::ToString(display);
    _aidl_os << ", layers: " << ::android::internal::ToString(layers);
    _aidl_os << ", colorTransformMatrix: " << ::android::internal::ToString(colorTransformMatrix);
    _aidl_os << ", brightness: " << ::android::internal::ToString(brightness);
    _aidl_os << ", clientTarget: " << ::android::internal::ToString(clientTarget);
    _aidl_os << ", virtualDisplayOutputBuffer: " << ::android::internal::ToString(virtualDisplayOutputBuffer);
    _aidl_os << ", expectedPresentTime: " << ::android::internal::ToString(expectedPresentTime);
    _aidl_os << ", validateDisplay: " << ::android::internal::ToString(validateDisplay);
    _aidl_os << ", acceptDisplayChanges: " << ::android::internal::ToString(acceptDisplayChanges);
    _aidl_os << ", presentDisplay: " << ::android::internal::ToString(presentDisplay);
    _aidl_os << ", presentOrValidateDisplay: " << ::android::internal::ToString(presentOrValidateDisplay);
    _aidl_os << ", frameIntervalNs: " << ::android::internal::ToString(frameIntervalNs);
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
