cmake_minimum_required(VERSION 3.20)
project(android_graphics_probe CXX)

set(LCL_ANDROID_PROBE_LIB_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../out/tools/android-graphics-probe-libs"
    CACHE PATH "Directory containing Android probe shared libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(android_graphics_probe
    main.cpp
    egl_probe.cpp
    ahb_probe.cpp
    ipc_probe.cpp
    composer_probe.cpp
    direct_presentation_probe.cpp
    onscreen_test_pattern.cpp
)

target_include_directories(android_graphics_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(android_graphics_probe PRIVATE
    EGL
    GLESv3
    android
    nativewindow
    binder_ndk
    ${LCL_ANDROID_PROBE_LIB_DIR}/android.hardware.graphics.composer3-V4-ndk.so
    ${LCL_ANDROID_PROBE_LIB_DIR}/android.hardware.graphics.common-V6-ndk.so
    ${LCL_ANDROID_PROBE_LIB_DIR}/android.hardware.common-V2-ndk.so
    dl
)
