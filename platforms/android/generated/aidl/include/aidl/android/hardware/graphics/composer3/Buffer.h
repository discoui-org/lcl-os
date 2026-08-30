/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/Buffer.aidl
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
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::common {
class NativeHandle;
}  // namespace aidl::android::hardware::common
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class Buffer {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  int32_t slot = 0;
  std::optional<::aidl::android::hardware::common::NativeHandle> handle;
  ::ndk::ScopedFileDescriptor fence;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const Buffer& _rhs) const {
    return std::tie(slot, handle, fence) == std::tie(_rhs.slot, _rhs.handle, _rhs.fence);
  }
  inline bool operator<(const Buffer& _rhs) const {
    return std::tie(slot, handle, fence) < std::tie(_rhs.slot, _rhs.handle, _rhs.fence);
  }
  inline bool operator!=(const Buffer& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const Buffer& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const Buffer& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const Buffer& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "Buffer{";
    _aidl_os << "slot: " << ::android::internal::ToString(slot);
    _aidl_os << ", handle: " << ::android::internal::ToString(handle);
    _aidl_os << ", fence: " << ::android::internal::ToString(fence);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
