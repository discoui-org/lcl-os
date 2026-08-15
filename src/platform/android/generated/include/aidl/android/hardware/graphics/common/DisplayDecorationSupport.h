/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/graphics_common/android/hardware/graphics/common/DisplayDecorationSupport.aidl
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
#include <aidl/android/hardware/graphics/common/AlphaInterpretation.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace common {
class DisplayDecorationSupport {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  ::aidl::android::hardware::graphics::common::PixelFormat format = ::aidl::android::hardware::graphics::common::PixelFormat(0);
  ::aidl::android::hardware::graphics::common::AlphaInterpretation alphaInterpretation = ::aidl::android::hardware::graphics::common::AlphaInterpretation(0);

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const DisplayDecorationSupport& _rhs) const {
    return std::tie(format, alphaInterpretation) == std::tie(_rhs.format, _rhs.alphaInterpretation);
  }
  inline bool operator<(const DisplayDecorationSupport& _rhs) const {
    return std::tie(format, alphaInterpretation) < std::tie(_rhs.format, _rhs.alphaInterpretation);
  }
  inline bool operator!=(const DisplayDecorationSupport& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const DisplayDecorationSupport& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const DisplayDecorationSupport& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const DisplayDecorationSupport& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "DisplayDecorationSupport{";
    _aidl_os << "format: " << ::android::internal::ToString(format);
    _aidl_os << ", alphaInterpretation: " << ::android::internal::ToString(alphaInterpretation);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace common
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
