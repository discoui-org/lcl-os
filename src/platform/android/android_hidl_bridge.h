#pragma once

#include <stdint.h>

struct AHardwareBuffer;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LclAndroidHidlDisplayInfo {
    int32_t width;
    int32_t height;
    int32_t refresh_rate_hz;
    float scale_factor;
    int64_t display_id;
    int64_t layer_id;
    int32_t connected;
    char name[160];
} LclAndroidHidlDisplayInfo;

void* lcl_android_hidl_create(void);
void lcl_android_hidl_destroy(void* instance);
int lcl_android_hidl_initialize(void* instance, float output_scale,
                                uint32_t preferred_width,
                                uint32_t preferred_height,
                                uint32_t preferred_refresh_hz,
                                LclAndroidHidlDisplayInfo* info);
void lcl_android_hidl_shutdown(void* instance);
int lcl_android_hidl_prepare_buffer(void* instance,
                                    struct AHardwareBuffer* buffer);
int lcl_android_hidl_present(void* instance, struct AHardwareBuffer* buffer,
                             int acquire_fence_fd);

#ifdef __cplusplus
}
#endif
