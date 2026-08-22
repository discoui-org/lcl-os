/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/VrrConfig.aidl
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
#include <android/binder_parcelable_utils.h>
#include <android/binder_to_string.h>
#include <aidl/android/hardware/graphics/composer3/VrrConfig.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class VrrConfig {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  class FrameIntervalPowerHint {
  public:
    typedef std::false_type fixed_size;
    static const char* descriptor;

    int32_t frameIntervalNs = 0;
    int32_t averageRefreshPeriodNs = 0;

    binder_status_t readFromParcel(const AParcel* parcel);
    binder_status_t writeToParcel(AParcel* parcel) const;

    inline bool operator==(const FrameIntervalPowerHint& _rhs) const {
      return std::tie(frameIntervalNs, averageRefreshPeriodNs) == std::tie(_rhs.frameIntervalNs, _rhs.averageRefreshPeriodNs);
    }
    inline bool operator<(const FrameIntervalPowerHint& _rhs) const {
      return std::tie(frameIntervalNs, averageRefreshPeriodNs) < std::tie(_rhs.frameIntervalNs, _rhs.averageRefreshPeriodNs);
    }
    inline bool operator!=(const FrameIntervalPowerHint& _rhs) const {
      return !(*this == _rhs);
    }
    inline bool operator>(const FrameIntervalPowerHint& _rhs) const {
      return _rhs < *this;
    }
    inline bool operator>=(const FrameIntervalPowerHint& _rhs) const {
      return !(*this < _rhs);
    }
    inline bool operator<=(const FrameIntervalPowerHint& _rhs) const {
      return !(_rhs < *this);
    }

    static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
    inline std::string toString() const {
      std::ostringstream _aidl_os;
      _aidl_os << "FrameIntervalPowerHint{";
      _aidl_os << "frameIntervalNs: " << ::android::internal::ToString(frameIntervalNs);
      _aidl_os << ", averageRefreshPeriodNs: " << ::android::internal::ToString(averageRefreshPeriodNs);
      _aidl_os << "}";
      return _aidl_os.str();
    }
  };
  class NotifyExpectedPresentConfig {
  public:
    typedef std::false_type fixed_size;
    static const char* descriptor;

    int32_t headsUpNs = 0;
    int32_t timeoutNs = 0;

    binder_status_t readFromParcel(const AParcel* parcel);
    binder_status_t writeToParcel(AParcel* parcel) const;

    inline bool operator==(const NotifyExpectedPresentConfig& _rhs) const {
      return std::tie(headsUpNs, timeoutNs) == std::tie(_rhs.headsUpNs, _rhs.timeoutNs);
    }
    inline bool operator<(const NotifyExpectedPresentConfig& _rhs) const {
      return std::tie(headsUpNs, timeoutNs) < std::tie(_rhs.headsUpNs, _rhs.timeoutNs);
    }
    inline bool operator!=(const NotifyExpectedPresentConfig& _rhs) const {
      return !(*this == _rhs);
    }
    inline bool operator>(const NotifyExpectedPresentConfig& _rhs) const {
      return _rhs < *this;
    }
    inline bool operator>=(const NotifyExpectedPresentConfig& _rhs) const {
      return !(*this < _rhs);
    }
    inline bool operator<=(const NotifyExpectedPresentConfig& _rhs) const {
      return !(_rhs < *this);
    }

    static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
    inline std::string toString() const {
      std::ostringstream _aidl_os;
      _aidl_os << "NotifyExpectedPresentConfig{";
      _aidl_os << "headsUpNs: " << ::android::internal::ToString(headsUpNs);
      _aidl_os << ", timeoutNs: " << ::android::internal::ToString(timeoutNs);
      _aidl_os << "}";
      return _aidl_os.str();
    }
  };
  int32_t minFrameIntervalNs = 0;
  std::optional<std::vector<std::optional<::aidl::android::hardware::graphics::composer3::VrrConfig::FrameIntervalPowerHint>>> frameIntervalPowerHints;
  std::optional<::aidl::android::hardware::graphics::composer3::VrrConfig::NotifyExpectedPresentConfig> notifyExpectedPresentConfig;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const VrrConfig& _rhs) const {
    return std::tie(minFrameIntervalNs, frameIntervalPowerHints, notifyExpectedPresentConfig) == std::tie(_rhs.minFrameIntervalNs, _rhs.frameIntervalPowerHints, _rhs.notifyExpectedPresentConfig);
  }
  inline bool operator<(const VrrConfig& _rhs) const {
    return std::tie(minFrameIntervalNs, frameIntervalPowerHints, notifyExpectedPresentConfig) < std::tie(_rhs.minFrameIntervalNs, _rhs.frameIntervalPowerHints, _rhs.notifyExpectedPresentConfig);
  }
  inline bool operator!=(const VrrConfig& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const VrrConfig& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const VrrConfig& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const VrrConfig& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "VrrConfig{";
    _aidl_os << "minFrameIntervalNs: " << ::android::internal::ToString(minFrameIntervalNs);
    _aidl_os << ", frameIntervalPowerHints: " << ::android::internal::ToString(frameIntervalPowerHints);
    _aidl_os << ", notifyExpectedPresentConfig: " << ::android::internal::ToString(notifyExpectedPresentConfig);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
