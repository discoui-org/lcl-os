/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/DisplayConfiguration.aidl
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
#include <aidl/android/hardware/graphics/composer3/DisplayConfiguration.h>
#include <aidl/android/hardware/graphics/composer3/OutputType.h>
#include <aidl/android/hardware/graphics/composer3/VrrConfig.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::graphics::composer3 {
class VrrConfig;
}  // namespace aidl::android::hardware::graphics::composer3
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class DisplayConfiguration {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  class Dpi {
  public:
    typedef std::false_type fixed_size;
    static const char* descriptor;

    float x = 0.000000f;
    float y = 0.000000f;

    binder_status_t readFromParcel(const AParcel* parcel);
    binder_status_t writeToParcel(AParcel* parcel) const;

    inline bool operator==(const Dpi& _rhs) const {
      return std::tie(x, y) == std::tie(_rhs.x, _rhs.y);
    }
    inline bool operator<(const Dpi& _rhs) const {
      return std::tie(x, y) < std::tie(_rhs.x, _rhs.y);
    }
    inline bool operator!=(const Dpi& _rhs) const {
      return !(*this == _rhs);
    }
    inline bool operator>(const Dpi& _rhs) const {
      return _rhs < *this;
    }
    inline bool operator>=(const Dpi& _rhs) const {
      return !(*this < _rhs);
    }
    inline bool operator<=(const Dpi& _rhs) const {
      return !(_rhs < *this);
    }

    static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
    inline std::string toString() const {
      std::ostringstream _aidl_os;
      _aidl_os << "Dpi{";
      _aidl_os << "x: " << ::android::internal::ToString(x);
      _aidl_os << ", y: " << ::android::internal::ToString(y);
      _aidl_os << "}";
      return _aidl_os.str();
    }
  };
  int32_t configId = 0;
  int32_t width = 0;
  int32_t height = 0;
  std::optional<::aidl::android::hardware::graphics::composer3::DisplayConfiguration::Dpi> dpi;
  int32_t configGroup = 0;
  int32_t vsyncPeriod = 0;
  std::optional<::aidl::android::hardware::graphics::composer3::VrrConfig> vrrConfig;
  ::aidl::android::hardware::graphics::composer3::OutputType hdrOutputType = ::aidl::android::hardware::graphics::composer3::OutputType(0);

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const DisplayConfiguration& _rhs) const {
    return std::tie(configId, width, height, dpi, configGroup, vsyncPeriod, vrrConfig, hdrOutputType) == std::tie(_rhs.configId, _rhs.width, _rhs.height, _rhs.dpi, _rhs.configGroup, _rhs.vsyncPeriod, _rhs.vrrConfig, _rhs.hdrOutputType);
  }
  inline bool operator<(const DisplayConfiguration& _rhs) const {
    return std::tie(configId, width, height, dpi, configGroup, vsyncPeriod, vrrConfig, hdrOutputType) < std::tie(_rhs.configId, _rhs.width, _rhs.height, _rhs.dpi, _rhs.configGroup, _rhs.vsyncPeriod, _rhs.vrrConfig, _rhs.hdrOutputType);
  }
  inline bool operator!=(const DisplayConfiguration& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const DisplayConfiguration& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const DisplayConfiguration& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const DisplayConfiguration& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "DisplayConfiguration{";
    _aidl_os << "configId: " << ::android::internal::ToString(configId);
    _aidl_os << ", width: " << ::android::internal::ToString(width);
    _aidl_os << ", height: " << ::android::internal::ToString(height);
    _aidl_os << ", dpi: " << ::android::internal::ToString(dpi);
    _aidl_os << ", configGroup: " << ::android::internal::ToString(configGroup);
    _aidl_os << ", vsyncPeriod: " << ::android::internal::ToString(vsyncPeriod);
    _aidl_os << ", vrrConfig: " << ::android::internal::ToString(vrrConfig);
    _aidl_os << ", hdrOutputType: " << ::android::internal::ToString(hdrOutputType);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
