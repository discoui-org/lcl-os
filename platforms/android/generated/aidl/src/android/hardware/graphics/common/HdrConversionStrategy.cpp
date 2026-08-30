/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/graphics_common/android/hardware/graphics/common/HdrConversionStrategy.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#include "aidl/android/hardware/graphics/common/HdrConversionStrategy.h"

#include <cstdint>
#include <android/binder_parcel.h>
#include <android/binder_parcel_utils.h>
#include <android/binder_status.h>

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace common {
const char* HdrConversionStrategy::descriptor = "android.hardware.graphics.common.HdrConversionStrategy";

binder_status_t HdrConversionStrategy::readFromParcel(const AParcel* _parcel) {
  binder_status_t _aidl_ret_status;
  int32_t _aidl_tag;
  if ((_aidl_ret_status = ::ndk::AParcel_readData(_parcel, &_aidl_tag)) != STATUS_OK) return _aidl_ret_status;
  switch (static_cast<Tag>(_aidl_tag)) {
  case passthrough: {
    bool _aidl_value;
    if ((_aidl_ret_status = ::ndk::AParcel_readData(_parcel, &_aidl_value)) != STATUS_OK) return _aidl_ret_status;
    if constexpr (std::is_trivially_copyable_v<bool>) {
      set<passthrough>(_aidl_value);
    } else {
      // NOLINTNEXTLINE(performance-move-const-arg)
      set<passthrough>(std::move(_aidl_value));
    }
    return STATUS_OK; }
  case autoAllowedHdrTypes: {
    std::vector<::aidl::android::hardware::graphics::common::Hdr> _aidl_value;
    if ((_aidl_ret_status = ::ndk::AParcel_readData(_parcel, &_aidl_value)) != STATUS_OK) return _aidl_ret_status;
    if constexpr (std::is_trivially_copyable_v<std::vector<::aidl::android::hardware::graphics::common::Hdr>>) {
      set<autoAllowedHdrTypes>(_aidl_value);
    } else {
      // NOLINTNEXTLINE(performance-move-const-arg)
      set<autoAllowedHdrTypes>(std::move(_aidl_value));
    }
    return STATUS_OK; }
  case forceHdrConversion: {
    ::aidl::android::hardware::graphics::common::Hdr _aidl_value;
    if ((_aidl_ret_status = ::ndk::AParcel_readData(_parcel, &_aidl_value)) != STATUS_OK) return _aidl_ret_status;
    if constexpr (std::is_trivially_copyable_v<::aidl::android::hardware::graphics::common::Hdr>) {
      set<forceHdrConversion>(_aidl_value);
    } else {
      // NOLINTNEXTLINE(performance-move-const-arg)
      set<forceHdrConversion>(std::move(_aidl_value));
    }
    return STATUS_OK; }
  }
  return STATUS_BAD_VALUE;
}
binder_status_t HdrConversionStrategy::writeToParcel(AParcel* _parcel) const {
  binder_status_t _aidl_ret_status = ::ndk::AParcel_writeData(_parcel, static_cast<int32_t>(getTag()));
  if (_aidl_ret_status != STATUS_OK) return _aidl_ret_status;
  switch (getTag()) {
  case passthrough: return ::ndk::AParcel_writeData(_parcel, get<passthrough>());
  case autoAllowedHdrTypes: return ::ndk::AParcel_writeData(_parcel, get<autoAllowedHdrTypes>());
  case forceHdrConversion: return ::ndk::AParcel_writeData(_parcel, get<forceHdrConversion>());
  }
  __assert2(__FILE__, __LINE__, __PRETTY_FUNCTION__, "can't reach here");
}

}  // namespace common
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
