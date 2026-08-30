/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/IComposer.aidl
 *
 * DO NOT CHECK THIS FILE INTO A CODE TREE (e.g. git, etc..).
 * ALWAYS GENERATE THIS FILE FROM UPDATED AIDL COMPILER
 * AS A BUILD INTERMEDIATE ONLY. THIS IS NOT SOURCE CODE.
 */
#pragma once

#include "aidl/android/hardware/graphics/composer3/IComposer.h"

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
class BnComposer : public ::ndk::BnCInterface<IComposer> {
public:
  BnComposer();
  virtual ~BnComposer();
protected:
  ::ndk::SpAIBinder createBinder() override;
private:
};
class IComposerDelegator : public BnComposer {
public:
  explicit IComposerDelegator(const std::shared_ptr<IComposer> &impl) : _impl(impl) {
  }

  ::ndk::ScopedAStatus createClient(std::shared_ptr<::aidl::android::hardware::graphics::composer3::IComposerClient>* _aidl_return) override {
    return _impl->createClient(_aidl_return);
  }
  ::ndk::ScopedAStatus getCapabilities(std::vector<::aidl::android::hardware::graphics::composer3::Capability>* _aidl_return) override {
    return _impl->getCapabilities(_aidl_return);
  }
protected:
private:
  std::shared_ptr<IComposer> _impl;
};

}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
