/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/drm_common/android/hardware/drm/HdcpLevels.aidl
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
#include <aidl/android/hardware/drm/HdcpLevel.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace drm {
class HdcpLevels {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  ::aidl::android::hardware::drm::HdcpLevel connectedLevel = ::aidl::android::hardware::drm::HdcpLevel(0);
  ::aidl::android::hardware::drm::HdcpLevel maxLevel = ::aidl::android::hardware::drm::HdcpLevel(0);

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const HdcpLevels& _rhs) const {
    return std::tie(connectedLevel, maxLevel) == std::tie(_rhs.connectedLevel, _rhs.maxLevel);
  }
  inline bool operator<(const HdcpLevels& _rhs) const {
    return std::tie(connectedLevel, maxLevel) < std::tie(_rhs.connectedLevel, _rhs.maxLevel);
  }
  inline bool operator!=(const HdcpLevels& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const HdcpLevels& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const HdcpLevels& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const HdcpLevels& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "HdcpLevels{";
    _aidl_os << "connectedLevel: " << ::android::internal::ToString(connectedLevel);
    _aidl_os << ", maxLevel: " << ::android::internal::ToString(maxLevel);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace drm
}  // namespace hardware
}  // namespace android
}  // namespace aidl
