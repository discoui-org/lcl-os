/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/graphics_common/android/hardware/graphics/common/DisplayHotplugEvent.aidl
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
enum class DisplayHotplugEvent : int32_t {
  CONNECTED = 0,
  DISCONNECTED = 1,
  ERROR_UNKNOWN = -1,
  ERROR_INCOMPATIBLE_CABLE = -2,
  ERROR_TOO_MANY_DISPLAYS = -3,
  ERROR_LINK_UNSTABLE = -4,
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
[[nodiscard]] static inline std::string toString(DisplayHotplugEvent val) {
  switch(val) {
  case DisplayHotplugEvent::CONNECTED:
    return "CONNECTED";
  case DisplayHotplugEvent::DISCONNECTED:
    return "DISCONNECTED";
  case DisplayHotplugEvent::ERROR_UNKNOWN:
    return "ERROR_UNKNOWN";
  case DisplayHotplugEvent::ERROR_INCOMPATIBLE_CABLE:
    return "ERROR_INCOMPATIBLE_CABLE";
  case DisplayHotplugEvent::ERROR_TOO_MANY_DISPLAYS:
    return "ERROR_TOO_MANY_DISPLAYS";
  case DisplayHotplugEvent::ERROR_LINK_UNSTABLE:
    return "ERROR_LINK_UNSTABLE";
  default:
    return std::to_string(static_cast<int32_t>(val));
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
constexpr inline std::array<aidl::android::hardware::graphics::common::DisplayHotplugEvent, 6> enum_values<aidl::android::hardware::graphics::common::DisplayHotplugEvent> = {
  aidl::android::hardware::graphics::common::DisplayHotplugEvent::CONNECTED,
  aidl::android::hardware::graphics::common::DisplayHotplugEvent::DISCONNECTED,
  aidl::android::hardware::graphics::common::DisplayHotplugEvent::ERROR_UNKNOWN,
  aidl::android::hardware::graphics::common::DisplayHotplugEvent::ERROR_INCOMPATIBLE_CABLE,
  aidl::android::hardware::graphics::common::DisplayHotplugEvent::ERROR_TOO_MANY_DISPLAYS,
  aidl::android::hardware::graphics::common::DisplayHotplugEvent::ERROR_LINK_UNSTABLE,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
