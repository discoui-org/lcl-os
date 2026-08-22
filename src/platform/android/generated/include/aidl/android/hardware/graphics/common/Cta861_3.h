/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/graphics_common/android/hardware/graphics/common/Cta861_3.aidl
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
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace common {
class Cta861_3 {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  float maxContentLightLevel = 0.000000f;
  float maxFrameAverageLightLevel = 0.000000f;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const Cta861_3& _rhs) const {
    return std::tie(maxContentLightLevel, maxFrameAverageLightLevel) == std::tie(_rhs.maxContentLightLevel, _rhs.maxFrameAverageLightLevel);
  }
  inline bool operator<(const Cta861_3& _rhs) const {
    return std::tie(maxContentLightLevel, maxFrameAverageLightLevel) < std::tie(_rhs.maxContentLightLevel, _rhs.maxFrameAverageLightLevel);
  }
  inline bool operator!=(const Cta861_3& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const Cta861_3& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const Cta861_3& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const Cta861_3& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "Cta861_3{";
    _aidl_os << "maxContentLightLevel: " << ::android::internal::ToString(maxContentLightLevel);
    _aidl_os << ", maxFrameAverageLightLevel: " << ::android::internal::ToString(maxFrameAverageLightLevel);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace common
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
