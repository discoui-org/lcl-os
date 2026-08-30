/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/graphics_common/android/hardware/graphics/common/ChromaSiting.aidl
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
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace common {
enum class ChromaSiting : int64_t {
  NONE = 0L,
  UNKNOWN = 1L,
  SITED_INTERSTITIAL = 2L,
  COSITED_HORIZONTAL = 3L,
  COSITED_VERTICAL = 4L,
  COSITED_BOTH = 5L,
};

}  // namespace common
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace common {
[[nodiscard]] static inline std::string toString(ChromaSiting val) {
  switch(val) {
  case ChromaSiting::NONE:
    return "NONE";
  case ChromaSiting::UNKNOWN:
    return "UNKNOWN";
  case ChromaSiting::SITED_INTERSTITIAL:
    return "SITED_INTERSTITIAL";
  case ChromaSiting::COSITED_HORIZONTAL:
    return "COSITED_HORIZONTAL";
  case ChromaSiting::COSITED_VERTICAL:
    return "COSITED_VERTICAL";
  case ChromaSiting::COSITED_BOTH:
    return "COSITED_BOTH";
  default:
    return std::to_string(static_cast<int64_t>(val));
  }
}
}  // namespace common
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
template <>
constexpr inline std::array<aidl::android::hardware::graphics::common::ChromaSiting, 6> enum_values<aidl::android::hardware::graphics::common::ChromaSiting> = {
  aidl::android::hardware::graphics::common::ChromaSiting::NONE,
  aidl::android::hardware::graphics::common::ChromaSiting::UNKNOWN,
  aidl::android::hardware::graphics::common::ChromaSiting::SITED_INTERSTITIAL,
  aidl::android::hardware::graphics::common::ChromaSiting::COSITED_HORIZONTAL,
  aidl::android::hardware::graphics::common::ChromaSiting::COSITED_VERTICAL,
  aidl::android::hardware::graphics::common::ChromaSiting::COSITED_BOTH,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
