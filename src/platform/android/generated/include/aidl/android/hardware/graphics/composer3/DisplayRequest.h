/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /home/superb/Android/Sdk/build-tools/36.1.0/aidl --lang=ndk --structured --stability=vintf -I /tmp/lcl_aidl_gen_avdNoa/composer -I /tmp/lcl_aidl_gen_avdNoa/graphics_common -I /tmp/lcl_aidl_gen_avdNoa/common -I /tmp/lcl_aidl_gen_avdNoa/drm_common -h /tmp/lcl_aidl_gen_avdNoa/out_inc -o /tmp/lcl_aidl_gen_avdNoa/out_src /tmp/lcl_aidl_gen_avdNoa/composer/android/hardware/graphics/composer3/DisplayRequest.aidl
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
#include <aidl/android/hardware/graphics/composer3/DisplayRequest.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace graphics {
namespace composer3 {
class DisplayRequest {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  class LayerRequest {
  public:
    typedef std::false_type fixed_size;
    static const char* descriptor;

    int64_t layer = 0L;
    int32_t mask = 0;

    binder_status_t readFromParcel(const AParcel* parcel);
    binder_status_t writeToParcel(AParcel* parcel) const;

    inline bool operator==(const LayerRequest& _rhs) const {
      return std::tie(layer, mask) == std::tie(_rhs.layer, _rhs.mask);
    }
    inline bool operator<(const LayerRequest& _rhs) const {
      return std::tie(layer, mask) < std::tie(_rhs.layer, _rhs.mask);
    }
    inline bool operator!=(const LayerRequest& _rhs) const {
      return !(*this == _rhs);
    }
    inline bool operator>(const LayerRequest& _rhs) const {
      return _rhs < *this;
    }
    inline bool operator>=(const LayerRequest& _rhs) const {
      return !(*this < _rhs);
    }
    inline bool operator<=(const LayerRequest& _rhs) const {
      return !(_rhs < *this);
    }

    static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
    enum : int32_t { CLEAR_CLIENT_TARGET = 1 };
    inline std::string toString() const {
      std::ostringstream _aidl_os;
      _aidl_os << "LayerRequest{";
      _aidl_os << "layer: " << ::android::internal::ToString(layer);
      _aidl_os << ", mask: " << ::android::internal::ToString(mask);
      _aidl_os << "}";
      return _aidl_os.str();
    }
  };
  int64_t display = 0L;
  int32_t mask = 0;
  std::vector<::aidl::android::hardware::graphics::composer3::DisplayRequest::LayerRequest> layerRequests;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator==(const DisplayRequest& _rhs) const {
    return std::tie(display, mask, layerRequests) == std::tie(_rhs.display, _rhs.mask, _rhs.layerRequests);
  }
  inline bool operator<(const DisplayRequest& _rhs) const {
    return std::tie(display, mask, layerRequests) < std::tie(_rhs.display, _rhs.mask, _rhs.layerRequests);
  }
  inline bool operator!=(const DisplayRequest& _rhs) const {
    return !(*this == _rhs);
  }
  inline bool operator>(const DisplayRequest& _rhs) const {
    return _rhs < *this;
  }
  inline bool operator>=(const DisplayRequest& _rhs) const {
    return !(*this < _rhs);
  }
  inline bool operator<=(const DisplayRequest& _rhs) const {
    return !(_rhs < *this);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  enum : int32_t { FLIP_CLIENT_TARGET = 1 };
  enum : int32_t { WRITE_CLIENT_TARGET_TO_OUTPUT = 2 };
  inline std::string toString() const {
    std::ostringstream _aidl_os;
    _aidl_os << "DisplayRequest{";
    _aidl_os << "display: " << ::android::internal::ToString(display);
    _aidl_os << ", mask: " << ::android::internal::ToString(mask);
    _aidl_os << ", layerRequests: " << ::android::internal::ToString(layerRequests);
    _aidl_os << "}";
    return _aidl_os.str();
  }
};
}  // namespace composer3
}  // namespace graphics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
