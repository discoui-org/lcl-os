/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/Luts.aidl
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
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::graphics::composer3 {
class LutProperties;
}  // namespace aidl::android::hardware::graphics::composer3
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class Luts {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  ::ndk::ScopedFileDescriptor pfd;
  std::optional<std::vector<int32_t>> offsets;
  std::vector<::aidl::android::hardware::graphics::composer3::LutProperties> lutProperties;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const Luts& _rhs) const {
    return std::tie(pfd, offsets, lutProperties) == std::tie(_rhs.pfd, _rhs.offsets, _rhs.lutProperties);
  }
  inline bool operator<(const Luts& _rhs) const {
    return std::tie(pfd, offsets, lutProperties) < std::tie(_rhs.pfd, _rhs.offsets, _rhs.lutProperties);
  }
  inline bool operator!=(const Luts& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const Luts& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const Luts& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const Luts& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "Luts{";
    _aidl_os << "pfd: " << ::android::internal::ToString(pfd);
    _aidl_os << ", offsets: " << ::android::internal::ToString(offsets);
    _aidl_os << ", lutProperties: " << ::android::internal::ToString(lutProperties);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
