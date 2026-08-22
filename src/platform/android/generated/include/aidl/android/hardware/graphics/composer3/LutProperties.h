/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/LutProperties.aidl
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
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class LutProperties {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class Dimension : int8_t {
    ONE_D = 1,
    THREE_D = 3,
  };

  enum class SamplingKey : int8_t {
    RGB = 0,
    MAX_RGB = 1,
    CIE_Y = 2,
  };

  ::aidl::android::hardware::graphics::composer3::LutProperties::Dimension dimension = ::aidl::android::hardware::graphics::composer3::LutProperties::Dimension(0);
  int32_t size = 0;
  std::vector<::aidl::android::hardware::graphics::composer3::LutProperties::SamplingKey> samplingKeys;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const LutProperties& _rhs) const {
    return std::tie(dimension, size, samplingKeys) == std::tie(_rhs.dimension, _rhs.size, _rhs.samplingKeys);
  }
  inline bool operator<(const LutProperties& _rhs) const {
    return std::tie(dimension, size, samplingKeys) < std::tie(_rhs.dimension, _rhs.size, _rhs.samplingKeys);
  }
  inline bool operator!=(const LutProperties& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const LutProperties& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const LutProperties& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const LutProperties& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "LutProperties{";
    _aidl_os << "dimension: " << ::android::internal::ToString(dimension);
    _aidl_os << ", size: " << ::android::internal::ToString(size);
    _aidl_os << ", samplingKeys: " << ::android::internal::ToString(samplingKeys);
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
[[nodiscard]] static inline std::string toString(LutProperties::Dimension val) {
  switch(val) {
  case LutProperties::Dimension::ONE_D:
    return "ONE_D";
  case LutProperties::Dimension::THREE_D:
    return "THREE_D";
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
constexpr inline std::array<aidl::android::hardware::graphics::composer3::LutProperties::Dimension, 2> enum_values<aidl::android::hardware::graphics::composer3::LutProperties::Dimension> = {
  aidl::android::hardware::graphics::composer3::LutProperties::Dimension::ONE_D,
  aidl::android::hardware::graphics::composer3::LutProperties::Dimension::THREE_D,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
[[nodiscard]] static inline std::string toString(LutProperties::SamplingKey val) {
  switch(val) {
  case LutProperties::SamplingKey::RGB:
    return "RGB";
  case LutProperties::SamplingKey::MAX_RGB:
    return "MAX_RGB";
  case LutProperties::SamplingKey::CIE_Y:
    return "CIE_Y";
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
constexpr inline std::array<aidl::android::hardware::graphics::composer3::LutProperties::SamplingKey, 3> enum_values<aidl::android::hardware::graphics::composer3::LutProperties::SamplingKey> = {
  aidl::android::hardware::graphics::composer3::LutProperties::SamplingKey::RGB,
  aidl::android::hardware::graphics::composer3::LutProperties::SamplingKey::MAX_RGB,
  aidl::android::hardware::graphics::composer3::LutProperties::SamplingKey::CIE_Y,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
