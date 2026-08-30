/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/OverlayProperties.aidl
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
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#include <aidl/android/hardware/graphics/composer3/OverlayProperties.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::graphics::composer3 {
class LutProperties;
}  // namespace aidl::android::hardware::graphics::composer3
namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class OverlayProperties {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  class SupportedBufferCombinations {
  public:
    typedef std::false_type fixed_size;
    static const char* descriptor;

    std::vector<::aidl::android::hardware::graphics::common::PixelFormat> pixelFormats;
    std::vector<::aidl::android::hardware::graphics::common::Dataspace> standards;
    std::vector<::aidl::android::hardware::graphics::common::Dataspace> transfers;
    std::vector<::aidl::android::hardware::graphics::common::Dataspace> ranges;

    binder_status_t readFromParcel(const AParcel* parcel);
    binder_status_t writeToParcel(AParcel* parcel) const;

    inline bool operator==(const SupportedBufferCombinations& _rhs) const {
      return std::tie(pixelFormats, standards, transfers, ranges) == std::tie(_rhs.pixelFormats, _rhs.standards, _rhs.transfers, _rhs.ranges);
    }
    inline bool operator<(const SupportedBufferCombinations& _rhs) const {
      return std::tie(pixelFormats, standards, transfers, ranges) < std::tie(_rhs.pixelFormats, _rhs.standards, _rhs.transfers, _rhs.ranges);
    }
    inline bool operator!=(const SupportedBufferCombinations& _rhs) const {
      return !(*this == _rhs);
    }
    inline bool operator>(const SupportedBufferCombinations& _rhs) const {
      return _rhs < *this;
    }
    inline bool operator>=(const SupportedBufferCombinations& _rhs) const {
      return !(*this < _rhs);
    }
    inline bool operator<=(const SupportedBufferCombinations& _rhs) const {
      return !(_rhs < *this);
    }

    static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
    inline std::string toString() const {
      std::ostringstream _aidl_os;
      _aidl_os << "SupportedBufferCombinations{";
      _aidl_os << "pixelFormats: " << ::android::internal::ToString(pixelFormats);
      _aidl_os << ", standards: " << ::android::internal::ToString(standards);
      _aidl_os << ", transfers: " << ::android::internal::ToString(transfers);
      _aidl_os << ", ranges: " << ::android::internal::ToString(ranges);
      _aidl_os << "}";
      return _aidl_os.str();
    }
  };
  std::vector<::aidl::android::hardware::graphics::composer3::OverlayProperties::SupportedBufferCombinations> combinations;
  bool supportMixedColorSpaces = false;
  std::optional<std::vector<std::optional<::aidl::android::hardware::graphics::composer3::LutProperties>>> lutProperties;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const OverlayProperties& _rhs) const {
    return std::tie(combinations, supportMixedColorSpaces, lutProperties) == std::tie(_rhs.combinations, _rhs.supportMixedColorSpaces, _rhs.lutProperties);
  }
  inline bool operator<(const OverlayProperties& _rhs) const {
    return std::tie(combinations, supportMixedColorSpaces, lutProperties) < std::tie(_rhs.combinations, _rhs.supportMixedColorSpaces, _rhs.lutProperties);
  }
  inline bool operator!=(const OverlayProperties& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const OverlayProperties& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const OverlayProperties& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const OverlayProperties& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "OverlayProperties{";
    _aidl_os << "combinations: " << ::android::internal::ToString(combinations);
    _aidl_os << ", supportMixedColorSpaces: " << ::android::internal::ToString(supportMixedColorSpaces);
    _aidl_os << ", lutProperties: " << ::android::internal::ToString(lutProperties);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
