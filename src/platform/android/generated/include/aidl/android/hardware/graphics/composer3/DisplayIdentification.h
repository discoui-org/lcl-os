/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/DisplayIdentification.aidl
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
#include <aidl/android/hardware/graphics/composer3/ScreenPartStatus.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class DisplayIdentification {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  int8_t port = 0;
  std::vector<uint8_t> data;
  ::aidl::android::hardware::graphics::composer3::ScreenPartStatus screenPartStatus = ::aidl::android::hardware::graphics::composer3::ScreenPartStatus::UNSUPPORTED;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const DisplayIdentification& _rhs) const {
    return std::tie(port, data, screenPartStatus) == std::tie(_rhs.port, _rhs.data, _rhs.screenPartStatus);
  }
  inline bool operator<(const DisplayIdentification& _rhs) const {
    return std::tie(port, data, screenPartStatus) < std::tie(_rhs.port, _rhs.data, _rhs.screenPartStatus);
  }
  inline bool operator!=(const DisplayIdentification& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const DisplayIdentification& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const DisplayIdentification& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const DisplayIdentification& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "DisplayIdentification{";
    _aidl_os << "port: " << ::android::internal::ToString(port);
    _aidl_os << ", data: " << ::android::internal::ToString(data);
    _aidl_os << ", screenPartStatus: " << ::android::internal::ToString(screenPartStatus);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
