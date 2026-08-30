/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/ClientTargetPropertyWithBrightness.aidl
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
#include <aidl/android/hardware/graphics/composer3/ClientTargetProperty.h>
#include <aidl/android/hardware/graphics/composer3/DimmingStage.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::graphics::composer3 {
class ClientTargetProperty;
}  // namespace aidl::android::hardware::graphics::composer3
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class ClientTargetPropertyWithBrightness {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  int64_t display = 0L;
  ::aidl::android::hardware::graphics::composer3::ClientTargetProperty clientTargetProperty;
  float brightness = 0.000000f;
  ::aidl::android::hardware::graphics::composer3::DimmingStage dimmingStage = ::aidl::android::hardware::graphics::composer3::DimmingStage(0);

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const ClientTargetPropertyWithBrightness& _rhs) const {
    return std::tie(display, clientTargetProperty, brightness, dimmingStage) == std::tie(_rhs.display, _rhs.clientTargetProperty, _rhs.brightness, _rhs.dimmingStage);
  }
  inline bool operator<(const ClientTargetPropertyWithBrightness& _rhs) const {
    return std::tie(display, clientTargetProperty, brightness, dimmingStage) < std::tie(_rhs.display, _rhs.clientTargetProperty, _rhs.brightness, _rhs.dimmingStage);
  }
  inline bool operator!=(const ClientTargetPropertyWithBrightness& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const ClientTargetPropertyWithBrightness& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const ClientTargetPropertyWithBrightness& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const ClientTargetPropertyWithBrightness& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "ClientTargetPropertyWithBrightness{";
    _aidl_os << "display: " << ::android::internal::ToString(display);
    _aidl_os << ", clientTargetProperty: " << ::android::internal::ToString(clientTargetProperty);
    _aidl_os << ", brightness: " << ::android::internal::ToString(brightness);
    _aidl_os << ", dimmingStage: " << ::android::internal::ToString(dimmingStage);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
