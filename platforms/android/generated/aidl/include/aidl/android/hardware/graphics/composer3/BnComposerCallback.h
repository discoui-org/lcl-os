/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/IComposerCallback.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include "aidl/android/hardware/graphics/composer3/IComposerCallback.h"

#include <android/binder_ibinder.h>
#include <cassert>

#ifndef __BIONIC__
#ifndef __assert2
#define __assert2(a,b,c,d) ((void)0)
#endif
#endif

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class BnComposerCallback : public ::ndk::BnCInterface<IComposerCallback> {
public:
  BnComposerCallback();
  virtual ~BnComposerCallback();
protected:
  ::ndk::SpAIBinder createBinder() override;
private:
};
class IComposerCallbackDelegator : public BnComposerCallback {
public:
  explicit IComposerCallbackDelegator(const std::shared_ptr<IComposerCallback> &impl) : _impl(impl) {
  }

  ::ndk::ScopedAStatus onHotplug(int64_t in_display, bool in_connected) override __attribute__((deprecated(": Use instead onHotplugEvent"))) {
    return _impl->onHotplug(in_display, in_connected);
  }
  ::ndk::ScopedAStatus onRefresh(int64_t in_display) override {
    return _impl->onRefresh(in_display);
  }
  ::ndk::ScopedAStatus onSeamlessPossible(int64_t in_display) override {
    return _impl->onSeamlessPossible(in_display);
  }
  ::ndk::ScopedAStatus onVsync(int64_t in_display, int64_t in_timestamp, int32_t in_vsyncPeriodNanos) override {
    return _impl->onVsync(in_display, in_timestamp, in_vsyncPeriodNanos);
  }
  ::ndk::ScopedAStatus onVsyncPeriodTimingChanged(int64_t in_display, const ::aidl::android::hardware::graphics::composer3::VsyncPeriodChangeTimeline& in_updatedTimeline) override {
    return _impl->onVsyncPeriodTimingChanged(in_display, in_updatedTimeline);
  }
  ::ndk::ScopedAStatus onVsyncIdle(int64_t in_display) override {
    return _impl->onVsyncIdle(in_display);
  }
  ::ndk::ScopedAStatus onRefreshRateChangedDebug(const ::aidl::android::hardware::graphics::composer3::RefreshRateChangedDebugData& in_data) override {
    return _impl->onRefreshRateChangedDebug(in_data);
  }
  ::ndk::ScopedAStatus onHotplugEvent(int64_t in_display, ::aidl::android::hardware::graphics::common::DisplayHotplugEvent in_event) override {
    return _impl->onHotplugEvent(in_display, in_event);
  }
  ::ndk::ScopedAStatus onHdcpLevelsChanged(int64_t in_display, const ::aidl::android::hardware::drm::HdcpLevels& in_levels) override {
    return _impl->onHdcpLevelsChanged(in_display, in_levels);
  }
protected:
private:
  std::shared_ptr<IComposerCallback> _impl;
};

}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
