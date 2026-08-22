/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/LayerLifecycleBatchCommandType.aidl
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
enum class LayerLifecycleBatchCommandType : int32_t {
  MODIFY = 0,
  CREATE = 1,
  DESTROY = 2,
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
[[nodiscard]] static inline std::string toString(LayerLifecycleBatchCommandType val) {
  switch(val) {
  case LayerLifecycleBatchCommandType::MODIFY:
    return "MODIFY";
  case LayerLifecycleBatchCommandType::CREATE:
    return "CREATE";
  case LayerLifecycleBatchCommandType::DESTROY:
    return "DESTROY";
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
constexpr inline std::array<aidl::android::hardware::graphics::composer3::LayerLifecycleBatchCommandType, 3> enum_values<aidl::android::hardware::graphics::composer3::LayerLifecycleBatchCommandType> = {
  aidl::android::hardware::graphics::composer3::LayerLifecycleBatchCommandType::MODIFY,
  aidl::android::hardware::graphics::composer3::LayerLifecycleBatchCommandType::CREATE,
  aidl::android::hardware::graphics::composer3::LayerLifecycleBatchCommandType::DESTROY,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
