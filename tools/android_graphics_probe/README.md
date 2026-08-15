# LCL Android Graphics & HAL Probe

Standalone native diagnostic tool to evaluate Android EGL, OpenGL ES, AHardwareBuffer allocation, EGLImage import, GPU rendering, zero-copy Unix domain socket IPC, and Composer3 (HWC3) Binder interface on Android AVD.

## Target Environment
- **Platform:** Android 16 (API 36 / Baklava)
- **Architecture:** `x86_64`
- **Compiler:** Clang 19 via Android NDK 28.2 (`28.2.13676358`)
- **Graphics:** Android Meta-EGL / Ranchu GLES 3.0 Translator

## Probed Subsystems
1. **EGL / GLES Probe:** Verifies EGL display acquisition, context creation, GLES 3.0 extensions, and host GPU acceleration.
2. **AHardwareBuffer & EGLImage:** Allocates an `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` buffer, creates `EGLImageKHR`, attaches to FBO, renders via GLES, and verifies lifecycle.
3. **AHardwareBuffer Unix Socket IPC:** Tests process-to-process zero-copy handle sharing via `AHardwareBuffer_sendHandleToUnixSocket` / `AHardwareBuffer_recvHandleFromUnixSocket`.
4. **Composer3 Binder NDK:** Queries `android.hardware.graphics.composer3.IComposer/default` via `libbinder_ndk.so`.

## Build Instructions
```bash
export ANDROID_NDK="/home/superb/Android/Sdk/ndk/28.2.13676358"

cmake -B build -S . \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=x86_64 \
      -DANDROID_PLATFORM=android-35 \
      -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

## Running on AVD
```bash
adb push build/android_graphics_probe /data/local/tmp/
adb shell "chmod 755 /data/local/tmp/android_graphics_probe"
adb shell "/data/local/tmp/android_graphics_probe"
```
