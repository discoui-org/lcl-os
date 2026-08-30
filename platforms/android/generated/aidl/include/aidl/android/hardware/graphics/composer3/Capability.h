/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/Capability.aidl
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
enum class Capability : int32_t {
  INVALID = 0,
  SIDEBAND_STREAM = 1,
  SKIP_CLIENT_COLOR_TRANSFORM = 2,
  PRESENT_FENCE_IS_NOT_RELIABLE = 3,
  SKIP_VALIDATE __attribute__((deprecated("- enabled by default."))) = 4,
  BOOT_DISPLAY_CONFIG = 5,
  HDR_OUTPUT_CONVERSION_CONFIG = 6,
  REFRESH_RATE_CHANGED_CALLBACK_DEBUG = 7,
  LAYER_LIFECYCLE_BATCH_COMMAND = 8,
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
[[nodiscard]] static inline std::string toString(Capability val) {
  switch(val) {
  case Capability::INVALID:
    return "INVALID";
  case Capability::SIDEBAND_STREAM:
    return "SIDEBAND_STREAM";
  case Capability::SKIP_CLIENT_COLOR_TRANSFORM:
    return "SKIP_CLIENT_COLOR_TRANSFORM";
  case Capability::PRESENT_FENCE_IS_NOT_RELIABLE:
    return "PRESENT_FENCE_IS_NOT_RELIABLE";
  case Capability::SKIP_VALIDATE:
    return "SKIP_VALIDATE";
  case Capability::BOOT_DISPLAY_CONFIG:
    return "BOOT_DISPLAY_CONFIG";
  case Capability::HDR_OUTPUT_CONVERSION_CONFIG:
    return "HDR_OUTPUT_CONVERSION_CONFIG";
  case Capability::REFRESH_RATE_CHANGED_CALLBACK_DEBUG:
    return "REFRESH_RATE_CHANGED_CALLBACK_DEBUG";
  case Capability::LAYER_LIFECYCLE_BATCH_COMMAND:
    return "LAYER_LIFECYCLE_BATCH_COMMAND";
  default:
    return std::to_string(static_cast<int32_t>(val));
  }
}
#pragma clang diagnostic pop
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
template <>
constexpr inline std::array<aidl::android::hardware::graphics::composer3::Capability, 9> enum_values<aidl::android::hardware::graphics::composer3::Capability> = {
  aidl::android::hardware::graphics::composer3::Capability::INVALID,
  aidl::android::hardware::graphics::composer3::Capability::SIDEBAND_STREAM,
  aidl::android::hardware::graphics::composer3::Capability::SKIP_CLIENT_COLOR_TRANSFORM,
  aidl::android::hardware::graphics::composer3::Capability::PRESENT_FENCE_IS_NOT_RELIABLE,
  aidl::android::hardware::graphics::composer3::Capability::SKIP_VALIDATE,
  aidl::android::hardware::graphics::composer3::Capability::BOOT_DISPLAY_CONFIG,
  aidl::android::hardware::graphics::composer3::Capability::HDR_OUTPUT_CONVERSION_CONFIG,
  aidl::android::hardware::graphics::composer3::Capability::REFRESH_RATE_CHANGED_CALLBACK_DEBUG,
  aidl::android::hardware::graphics::composer3::Capability::LAYER_LIFECYCLE_BATCH_COMMAND,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
