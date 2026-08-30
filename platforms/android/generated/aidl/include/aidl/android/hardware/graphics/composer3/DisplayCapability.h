/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/DisplayCapability.aidl
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
namespace composer3 {
enum class DisplayCapability : int32_t {
  INVALID = 0,
  SKIP_CLIENT_COLOR_TRANSFORM = 1,
  DOZE = 2,
  BRIGHTNESS = 3,
  PROTECTED_CONTENTS = 4,
  AUTO_LOW_LATENCY_MODE = 5,
  SUSPEND = 6,
  DISPLAY_IDLE_TIMER = 7,
  MULTI_THREADED_PRESENT = 8,
  PICTURE_PROCESSING = 9,
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
[[nodiscard]] static inline std::string toString(DisplayCapability val) {
  switch(val) {
  case DisplayCapability::INVALID:
    return "INVALID";
  case DisplayCapability::SKIP_CLIENT_COLOR_TRANSFORM:
    return "SKIP_CLIENT_COLOR_TRANSFORM";
  case DisplayCapability::DOZE:
    return "DOZE";
  case DisplayCapability::BRIGHTNESS:
    return "BRIGHTNESS";
  case DisplayCapability::PROTECTED_CONTENTS:
    return "PROTECTED_CONTENTS";
  case DisplayCapability::AUTO_LOW_LATENCY_MODE:
    return "AUTO_LOW_LATENCY_MODE";
  case DisplayCapability::SUSPEND:
    return "SUSPEND";
  case DisplayCapability::DISPLAY_IDLE_TIMER:
    return "DISPLAY_IDLE_TIMER";
  case DisplayCapability::MULTI_THREADED_PRESENT:
    return "MULTI_THREADED_PRESENT";
  case DisplayCapability::PICTURE_PROCESSING:
    return "PICTURE_PROCESSING";
  default:
    return std::to_string(static_cast<int32_t>(val));
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
constexpr inline std::array<aidl::android::hardware::graphics::composer3::DisplayCapability, 10> enum_values<aidl::android::hardware::graphics::composer3::DisplayCapability> = {
  aidl::android::hardware::graphics::composer3::DisplayCapability::INVALID,
  aidl::android::hardware::graphics::composer3::DisplayCapability::SKIP_CLIENT_COLOR_TRANSFORM,
  aidl::android::hardware::graphics::composer3::DisplayCapability::DOZE,
  aidl::android::hardware::graphics::composer3::DisplayCapability::BRIGHTNESS,
  aidl::android::hardware::graphics::composer3::DisplayCapability::PROTECTED_CONTENTS,
  aidl::android::hardware::graphics::composer3::DisplayCapability::AUTO_LOW_LATENCY_MODE,
  aidl::android::hardware::graphics::composer3::DisplayCapability::SUSPEND,
  aidl::android::hardware::graphics::composer3::DisplayCapability::DISPLAY_IDLE_TIMER,
  aidl::android::hardware::graphics::composer3::DisplayCapability::MULTI_THREADED_PRESENT,
  aidl::android::hardware::graphics::composer3::DisplayCapability::PICTURE_PROCESSING,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
