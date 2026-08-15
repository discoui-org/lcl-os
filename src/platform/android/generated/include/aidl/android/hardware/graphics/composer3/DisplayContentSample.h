/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/DisplayContentSample.aidl
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
namespace composer3 {
class DisplayContentSample {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  int64_t frameCount = 0L;
  std::vector<int64_t> sampleComponent0;
  std::vector<int64_t> sampleComponent1;
  std::vector<int64_t> sampleComponent2;
  std::vector<int64_t> sampleComponent3;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const DisplayContentSample& _rhs) const {
    return std::tie(frameCount, sampleComponent0, sampleComponent1, sampleComponent2, sampleComponent3) == std::tie(_rhs.frameCount, _rhs.sampleComponent0, _rhs.sampleComponent1, _rhs.sampleComponent2, _rhs.sampleComponent3);
  }
  inline bool operator<(const DisplayContentSample& _rhs) const {
    return std::tie(frameCount, sampleComponent0, sampleComponent1, sampleComponent2, sampleComponent3) < std::tie(_rhs.frameCount, _rhs.sampleComponent0, _rhs.sampleComponent1, _rhs.sampleComponent2, _rhs.sampleComponent3);
  }
  inline bool operator!=(const DisplayContentSample& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const DisplayContentSample& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const DisplayContentSample& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const DisplayContentSample& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "DisplayContentSample{";
    _aidl_os << "frameCount: " << ::android::internal::ToString(frameCount);
    _aidl_os << ", sampleComponent0: " << ::android::internal::ToString(sampleComponent0);
    _aidl_os << ", sampleComponent1: " << ::android::internal::ToString(sampleComponent1);
    _aidl_os << ", sampleComponent2: " << ::android::internal::ToString(sampleComponent2);
    _aidl_os << ", sampleComponent3: " << ::android::internal::ToString(sampleComponent3);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
