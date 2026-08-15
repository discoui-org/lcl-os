/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/IComposerCallback.aidl
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
#include <aidl/android/hardware/drm/HdcpLevels.h>
#include <aidl/android/hardware/graphics/common/DisplayHotplugEvent.h>
#include <aidl/android/hardware/graphics/composer3/RefreshRateChangedDebugData.h>
#include <aidl/android/hardware/graphics/composer3/VsyncPeriodChangeTimeline.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::drm {
class HdcpLevels;
}  // namespace aidl::android::hardware::drm
namespace aidl::android::hardware::graphics::composer3 {
class RefreshRateChangedDebugData;
class VsyncPeriodChangeTimeline;
}  // namespace aidl::android::hardware::graphics::composer3
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class IComposerCallbackDelegator;

class IComposerCallback : public ::ndk::ICInterface {
public:
  typedef IComposerCallbackDelegator DefaultDelegator;
  static const char* descriptor;
  IComposerCallback();
  virtual ~IComposerCallback();

  static constexpr uint32_t TRANSACTION_onHotplug = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_onRefresh = FIRST_CALL_TRANSACTION + 1;
  static constexpr uint32_t TRANSACTION_onSeamlessPossible = FIRST_CALL_TRANSACTION + 2;
  static constexpr uint32_t TRANSACTION_onVsync = FIRST_CALL_TRANSACTION + 3;
  static constexpr uint32_t TRANSACTION_onVsyncPeriodTimingChanged = FIRST_CALL_TRANSACTION + 4;
  static constexpr uint32_t TRANSACTION_onVsyncIdle = FIRST_CALL_TRANSACTION + 5;
  static constexpr uint32_t TRANSACTION_onRefreshRateChangedDebug = FIRST_CALL_TRANSACTION + 6;
  static constexpr uint32_t TRANSACTION_onHotplugEvent = FIRST_CALL_TRANSACTION + 7;
  static constexpr uint32_t TRANSACTION_onHdcpLevelsChanged = FIRST_CALL_TRANSACTION + 8;

  static std::shared_ptr<IComposerCallback> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IComposerCallback>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IComposerCallback>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IComposerCallback>& impl);
  static const std::shared_ptr<IComposerCallback>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus onHotplug(int64_t in_display, bool in_connected) __attribute__((deprecated(": Use instead onHotplugEvent"))) = 0;
  virtual ::ndk::ScopedAStatus onRefresh(int64_t in_display) = 0;
  virtual ::ndk::ScopedAStatus onSeamlessPossible(int64_t in_display) = 0;
  virtual ::ndk::ScopedAStatus onVsync(int64_t in_display, int64_t in_timestamp, int32_t in_vsyncPeriodNanos) = 0;
  virtual ::ndk::ScopedAStatus onVsyncPeriodTimingChanged(int64_t in_display, const ::aidl::android::hardware::graphics::composer3::VsyncPeriodChangeTimeline& in_updatedTimeline) = 0;
  virtual ::ndk::ScopedAStatus onVsyncIdle(int64_t in_display) = 0;
  virtual ::ndk::ScopedAStatus onRefreshRateChangedDebug(const ::aidl::android::hardware::graphics::composer3::RefreshRateChangedDebugData& in_data) = 0;
  virtual ::ndk::ScopedAStatus onHotplugEvent(int64_t in_display, ::aidl::android::hardware::graphics::common::DisplayHotplugEvent in_event) = 0;
  virtual ::ndk::ScopedAStatus onHdcpLevelsChanged(int64_t in_display, const ::aidl::android::hardware::drm::HdcpLevels& in_levels) = 0;
private:
  static std::shared_ptr<IComposerCallback> default_impl;
};
class IComposerCallbackDefault : public IComposerCallback {
public:
  ::ndk::ScopedAStatus onHotplug(int64_t in_display, bool in_connected) override __attribute__((deprecated(": Use instead onHotplugEvent")));
  ::ndk::ScopedAStatus onRefresh(int64_t in_display) override;
  ::ndk::ScopedAStatus onSeamlessPossible(int64_t in_display) override;
  ::ndk::ScopedAStatus onVsync(int64_t in_display, int64_t in_timestamp, int32_t in_vsyncPeriodNanos) override;
  ::ndk::ScopedAStatus onVsyncPeriodTimingChanged(int64_t in_display, const ::aidl::android::hardware::graphics::composer3::VsyncPeriodChangeTimeline& in_updatedTimeline) override;
  ::ndk::ScopedAStatus onVsyncIdle(int64_t in_display) override;
  ::ndk::ScopedAStatus onRefreshRateChangedDebug(const ::aidl::android::hardware::graphics::composer3::RefreshRateChangedDebugData& in_data) override;
  ::ndk::ScopedAStatus onHotplugEvent(int64_t in_display, ::aidl::android::hardware::graphics::common::DisplayHotplugEvent in_event) override;
  ::ndk::ScopedAStatus onHdcpLevelsChanged(int64_t in_display, const ::aidl::android::hardware::drm::HdcpLevels& in_levels) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
