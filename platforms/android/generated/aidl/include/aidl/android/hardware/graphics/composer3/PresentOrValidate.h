/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/PresentOrValidate.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_enums.h>
#include <android/binder_interface_utils.h>
#include <android/binder_parcelable_utils.h>
#include <android/binder_to_string.h>
#include <aidl/android/hardware/graphics/composer3/PresentOrValidate.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class PresentOrValidate {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class Result : int8_t {
    Validated = 0,
    Presented = 1,
  };

  int64_t display = 0L;
  ::aidl::android::hardware::graphics::composer3::PresentOrValidate::Result result = ::aidl::android::hardware::graphics::composer3::PresentOrValidate::Result(0);

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const PresentOrValidate& _rhs) const {
    return std::tie(display, result) == std::tie(_rhs.display, _rhs.result);
  }
  inline bool operator<(const PresentOrValidate& _rhs) const {
    return std::tie(display, result) < std::tie(_rhs.display, _rhs.result);
  }
  inline bool operator!=(const PresentOrValidate& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const PresentOrValidate& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const PresentOrValidate& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const PresentOrValidate& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "PresentOrValidate{";
    _aidl_os << "display: " << ::android::internal::ToString(display);
    _aidl_os << ", result: " << ::android::internal::ToString(result);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
[[nodiscard]] static inline std::string toString(PresentOrValidate::Result val) {
  switch(val) {
  case PresentOrValidate::Result::Validated:
    return "Validated";
  case PresentOrValidate::Result::Presented:
    return "Presented";
  default:
    return std::to_string(static_cast<int8_t>(val));
  }
}
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
template <>
constexpr inline std::array<aidl::android::hardware::graphics::composer3::PresentOrValidate::Result, 2> enum_values<aidl::android::hardware::graphics::composer3::PresentOrValidate::Result> = {
  aidl::android::hardware::graphics::composer3::PresentOrValidate::Result::Validated,
  aidl::android::hardware::graphics::composer3::PresentOrValidate::Result::Presented,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
