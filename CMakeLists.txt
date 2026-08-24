cmake_minimum_required(VERSION 3.20)
project(lcl-core VERSION 0.1.0 LANGUAGES C CXX)

# Require C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies
find_package(Threads REQUIRED)

if(NOT ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DRM libdrm)
    pkg_check_modules(GBM gbm)
    pkg_check_modules(EGL egl)
    pkg_check_modules(GLES2 glesv2)
    pkg_check_modules(LIBINPUT libinput)
    pkg_check_modules(UDEV libudev)
endif()

# QuickJS Engine (C-based JavaScript Runtime)
include(FetchContent)
FetchContent_Declare(
    quickjs
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.7.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BUILD_QJS_LIBC OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(quickjs)

# QuickJS owns a directory-wide -Werror policy.  Keep LCL targets strict, but
# do not let new host compiler diagnostics in vendored QuickJS tools fail the
# product build.
foreach(quickjs_target IN ITEMS qjs qjsc qjs_exe run-test262 function_source test_conv)
    if(TARGET ${quickjs_target})
        target_compile_options(${quickjs_target} PRIVATE -Wno-error)
    endif()
endforeach()

# Compiler flags for LCL OS core
add_compile_options(-Wall -Wextra -Wpedantic)

# Platform-agnostic interface library (consumed by core and render without concrete platform dependencies)
add_library(lcl-platform-common STATIC
    src/platform/common/gestalt.cpp
    src/platform/common/keyboard_mapper.cpp
    src/platform/common/output_scale.cpp
)
target_include_directories(lcl-platform-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Shared compositor/client IPC wire-format implementation.
add_library(lcl-protocol STATIC
    src/core/ipc/lcl_protocol.cpp
)

target_include_directories(lcl-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Backend-neutral logical geometry, path, paint and display-list contracts.
add_subdirectory(lcl-graphics)

# Backend-neutral semantic colors and native widget style values.
add_subdirectory(lcl-theme)

add_library(lcl-path-raster STATIC
    src/render/path_rasterizer.cpp
)
target_include_directories(lcl-path-raster PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(lcl-path-raster PUBLIC
    lcl-graphics
)

# Font selection and glyph-advance metrics are shared by lcl-ui layout and
# client/compositor rendering. Keeping this below the UI ABI avoids a renderer
# dependency from widgets while preserving one authoritative measurement path.
add_library(lcl-font-metrics STATIC
    src/render/font_renderer.cpp
    src/render/text_metrics.cpp
)

target_include_directories(lcl-font-metrics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
)

target_link_libraries(lcl-font-metrics PUBLIC
    lcl-graphics
)

# Session lifecycle transport and authority stay independent from compositor
# surface IPC.  This is the durable boundary between app processes and scenes.
add_library(lcl-session-protocol STATIC
    src/core/session/session_protocol.cpp
)

target_include_directories(lcl-session-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(lcl-session STATIC
    src/core/app/app_bundle_parser.cpp
    src/core/session/app_registry.cpp
    src/core/session/session_service.cpp
    src/core/session/session_client.cpp
)

target_include_directories(lcl-session PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lcl-session PUBLIC
    lcl-session-protocol
    Threads::Threads
)

# Typed revision-aware consumer for compositor-owned scene/focus state.  It has
# no widget dependency, so desktop and future mobile shells share one client.
add_library(lcl-shell-state STATIC
    src/core/shell/shell_state_client.cpp
    src/core/shell/shell_state_model.cpp
)

target_include_directories(lcl-shell-state PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lcl-shell-state PUBLIC
    lcl-protocol
)

# Renderer/UI-independent motion primitives shared by clients and compositor.
add_subdirectory(lcl-motion)

# Renderer-independent window chrome state shared by CSD clients and SSD.
add_subdirectory(lcl-window-chrome)

# Compositor-owned render/present layer.
# Depends strictly on lcl-platform-common; never on concrete platform layers.
add_library(lcl-render STATIC
    src/render/renderer.cpp
    src/render/raster_renderer.cpp
    src/render/window_manager.cpp
)

target_include_directories(lcl-render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(ANDROID)
    target_link_libraries(lcl-render PUBLIC
        lcl-motion
        lcl-graphics
        lcl-theme
        lcl-path-raster
        lcl-window-chrome
        lcl-protocol
        lcl-font-metrics
        lcl-platform-common
        GLESv3
        EGL
        android
        Threads::Threads
    )
else()
    target_include_directories(lcl-render PUBLIC
        ${GLES2_INCLUDE_DIRS}
    )
    target_link_libraries(lcl-render PUBLIC
        lcl-motion
        lcl-graphics
        lcl-theme
        lcl-path-raster
        lcl-window-chrome
        lcl-protocol
        lcl-font-metrics
        lcl-platform-common
        ${GLES2_LIBRARIES}
        Threads::Threads
    )
    target_compile_options(lcl-render PRIVATE
        ${GLES2_CFLAGS_OTHER}
    )
endif()

# Platform-agnostic Core Compositor Engine Library
add_library(lcl-core-engine STATIC
    src/core/compositor/compositor.cpp
    src/core/compositor/compositor_renderer.cpp
    src/core/compositor/frame_scheduler.cpp
    src/core/compositor/input_router.cpp
    src/core/compositor/protocol_dispatcher.cpp
    src/core/compositor/surface_registry.cpp
    src/core/compositor/system_surface_policy.cpp
    src/core/compositor/windowing_policy.cpp
    src/core/scene/focus_controller.cpp
    src/core/scene/scene_registry.cpp
    src/core/scene/shell_state_broker.cpp
    src/core/input/input_manager.cpp
    src/core/input/system_gesture_arena.cpp
    src/core/ipc/ipc_manager.cpp
)

target_include_directories(lcl-core-engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lcl-core-engine PUBLIC
    lcl-render
    lcl-platform-common
    lcl-protocol
    lcl-theme
    lcl-session
        lcl-motion
        lcl-graphics
    Threads::Threads
)

if(ANDROID)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

    # Android-native client raster target. Widgets and Canvas remain unchanged;
    # only this lowest graphics producer swaps GBM for AHardwareBuffer.
    add_library(lcl-raster STATIC
        src/render/client_egl_context.cpp
        src/render/raster_canvas.cpp
        src/render/raster_renderer.cpp
    )
    target_include_directories(lcl-raster PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )
    target_link_libraries(lcl-raster PUBLIC
        lcl-graphics
        lcl-path-raster
        lcl-font-metrics
        lcl-protocol
        EGL
        GLESv3
        android
        Threads::Threads
    )

    # Android-native clients reuse the exact widget/layout/Canvas sources. The
    # only target-specific code is the lcl-raster producer above.
    add_subdirectory(lcl-ui)

    set(LCL_ANDROID_HIDL_ROOT "" CACHE PATH
        "Prepared Android HIDL dependency root from scripts/prepare_android_hidl.py")
    if(LCL_ANDROID_HIDL_ROOT)
        include("${LCL_ANDROID_HIDL_ROOT}/hidl-config.cmake")
    endif()

    # Collect generated AIDL NDK sources
    file(GLOB_RECURSE ANDROID_AIDL_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/generated/src/*.cpp"
    )

    # Android Platform implementation library
    add_library(lcl-platform-android STATIC
        src/platform/android/android_input_backend.cpp
        src/platform/android/android_display_backend.cpp
        src/platform/android/android_hidl_display_backend.cpp
        src/platform/android/android_graphics_context.cpp
        src/platform/android/android_platform_services.cpp
        ${ANDROID_AIDL_SOURCES}
    )

    target_include_directories(lcl-platform-android PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/generated/include
    )

    target_link_libraries(lcl-platform-android PUBLIC
        lcl-platform-common
        binder_ndk
        EGL
        GLESv3
        android
        dl
    )

    if(LCL_ANDROID_HIDL_ROOT)
        # Platform HIDL uses Android's private libc++ ABI (std::__1), while NDK
        # applications use std::__ndk1.  Keep that ABI below a small C bridge.
        add_library(lcl-android-hidl-bridge SHARED
            src/platform/android/android_hidl_bridge.cpp
        )
        target_compile_definitions(lcl-android-hidl-bridge PRIVATE
            LCL_HAS_ANDROID_HIDL=1
        )
        target_include_directories(lcl-android-hidl-bridge BEFORE PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/hidl_libcpp_overlay
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/hidl_generated/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${LCL_ANDROID_HIDL_INCLUDE_DIRS}
        )
        target_compile_options(lcl-android-hidl-bridge PRIVATE
            -fno-rtti
            -Wno-deprecated-declarations
            -Wno-deprecated-copy-with-user-provided-copy
            -Wno-inconsistent-missing-override
            -Wno-format-pedantic
            -Wno-switch
        )
        target_link_options(lcl-android-hidl-bridge PRIVATE -nostdlib++)
        target_link_libraries(lcl-android-hidl-bridge PRIVATE
            ${LCL_ANDROID_HIDL_LIBRARIES}
            android
            dl
        )
        set_target_properties(lcl-android-hidl-bridge PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
        )
        add_dependencies(lcl-platform-android lcl-android-hidl-bridge)
    endif()

    # Standalone Platform Smoke Test
    add_executable(test_android_platform_services
        tests/android/test_android_platform_services.cpp
    )
    target_include_directories(test_android_platform_services PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/generated/include
    )
    target_link_libraries(test_android_platform_services PRIVATE
        lcl-platform-android
    )

    add_executable(test_android_client_raster
        tests/android/test_android_client_raster.cpp
    )
    target_include_directories(test_android_client_raster PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_android_client_raster PRIVATE
        lcl-raster
    )

    add_executable(test_android_window_client
        tests/android/test_android_window_client.cpp
    )
    target_include_directories(test_android_window_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )
    target_link_libraries(test_android_window_client PRIVATE
        lcl-ui
        lcl-raster
        lcl-protocol
        Threads::Threads
    )

    add_executable(lcl-desktop-shell
        src/apps/desktop_shell/ui_main.cpp
        src/core/ipc/ipc_manager.cpp
    )
    target_include_directories(lcl-desktop-shell PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )
    target_link_libraries(lcl-desktop-shell PRIVATE
        lcl-ui
        lcl-raster
        lcl-protocol
        lcl-shell-state
        lcl-session
        Threads::Threads
    )

    add_executable(lcl-mobile-shell
        src/apps/mobile_shell/ui_main.cpp
    )
    target_include_directories(lcl-mobile-shell PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )
    target_link_libraries(lcl-mobile-shell PRIVATE
        lcl-ui
        lcl-raster
        lcl-protocol
        Threads::Threads
    )

    add_executable(lcl-terminal
        src/tools/terminal_main.cpp
        src/apps/terminal/terminal_app.cpp
        src/apps/terminal/terminal_view.cpp
        src/core/terminal/pty_manager.cpp
    )
    target_include_directories(lcl-terminal PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )
    target_link_libraries(lcl-terminal PRIVATE
        lcl-ui
        lcl-raster
        lcl-protocol
        Threads::Threads
    )

    # Android Compositor Runtime (Composition Root)
    add_executable(lcl-core-android
        src/platform/android/main_android.cpp
    )
    target_include_directories(lcl-core-android PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/generated/include
    )
    target_link_libraries(lcl-core-android PRIVATE
        lcl-core-engine
        lcl-platform-android
    )
else()
    # =========================================================================
    # LCL Application Runtime & Canonical Executables
    #
    # ARCHITECTURAL INVARIANT:
    # For the same CPU architecture, LCL application executables are
    # platform-independent artifacts and must be reused byte-for-byte
    # across LCL platform targets.
    #
    # Platform differences terminate below the LCL userspace ABI.
    # =========================================================================

    # Backend-neutral widgets, layout, events and WindowApp lifecycle.
    add_subdirectory(lcl-ui)

    # Client canvas attempts a render-node-only EGL context and falls back to the
    # established SHM raster path.  It never owns compositor KMS presentation.
    add_library(lcl-raster STATIC
        src/render/client_egl_context.cpp
        src/render/raster_canvas.cpp
        src/render/raster_renderer.cpp
    )

    target_include_directories(lcl-raster PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
        ${DRM_INCLUDE_DIRS}
        ${GBM_INCLUDE_DIRS}
        ${EGL_INCLUDE_DIRS}
        ${GLES2_INCLUDE_DIRS}
    )

    target_link_libraries(lcl-raster PUBLIC
        lcl-graphics
        lcl-path-raster
        lcl-font-metrics
        lcl-protocol
        ${DRM_LIBRARIES}
        ${GBM_LIBRARIES}
        ${EGL_LIBRARIES}
        ${GLES2_LIBRARIES}
        Threads::Threads
    )

    target_compile_options(lcl-raster PRIVATE
        ${DRM_CFLAGS_OTHER}
        ${GBM_CFLAGS_OTHER}
        ${EGL_CFLAGS_OTHER}
        ${GLES2_CFLAGS_OTHER}
    )

    # Desktop Linux platform implementation library
    add_library(lcl-platform-desktop STATIC
        src/platform/desktop/drm_display_backend.cpp
        src/platform/desktop/gbm_graphics_context.cpp
        src/platform/desktop/evdev_input_backend.cpp
        src/platform/desktop/evdev_key_mapper.cpp
        src/platform/desktop/desktop_runtime_paths.cpp
        src/platform/desktop/desktop_platform_services.cpp
    )

    target_include_directories(lcl-platform-desktop PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DRM_INCLUDE_DIRS}
        ${GBM_INCLUDE_DIRS}
        ${EGL_INCLUDE_DIRS}
        ${GLES2_INCLUDE_DIRS}
        ${LIBINPUT_INCLUDE_DIRS}
        ${UDEV_INCLUDE_DIRS}
    )

    target_link_libraries(lcl-platform-desktop PUBLIC
        lcl-platform-common
        ${DRM_LIBRARIES}
        ${GBM_LIBRARIES}
        ${EGL_LIBRARIES}
        ${GLES2_LIBRARIES}
        ${LIBINPUT_LIBRARIES}
        ${UDEV_LIBRARIES}
    )

    target_compile_options(lcl-platform-desktop PRIVATE
        ${DRM_CFLAGS_OTHER}
        ${GBM_CFLAGS_OTHER}
        ${EGL_CFLAGS_OTHER}
        ${GLES2_CFLAGS_OTHER}
        ${LIBINPUT_CFLAGS_OTHER}
        ${UDEV_CFLAGS_OTHER}
    )

    # Main Compositor Executable Target (Desktop Composition Root)
    add_executable(lcl-core
        src/main.cpp
    )

    target_include_directories(lcl-core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(lcl-core PRIVATE
        lcl-core-engine
        lcl-platform-desktop
    )

    target_compile_options(lcl-core PRIVATE
        ${LIBINPUT_CFLAGS_OTHER}
        ${UDEV_CFLAGS_OTHER}
    )

    # Session authority: app catalog, launch, and process lifecycle.
    add_executable(lcl-sessiond
        src/tools/sessiond_main.cpp
    )

    target_include_directories(lcl-sessiond PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(lcl-sessiond PRIVATE
        lcl-session
        Threads::Threads
    )

    # Selects exactly one installed shell from the canonical Gestalt profile,
    # then replaces itself with that shell process.
    add_executable(lcl-shell-launcher
        src/tools/shell_launcher_main.cpp
    )
    target_include_directories(lcl-shell-launcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(lcl-shell-launcher PRIVATE
        lcl-platform-common
    )

    # Native CLI App Launcher Target (lcl-open). It never forks applications;
    # all process lifecycle is delegated to lcl-sessiond.
    add_executable(lcl-open
        src/tools/open_main.cpp
    )

    target_include_directories(lcl-open PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(lcl-open PRIVATE
        lcl-session
    )

    # Standalone Desktop Shell Daemon Target (lcl-desktop-shell)
    add_executable(lcl-desktop-shell
        src/apps/desktop_shell/ui_main.cpp
        src/core/ipc/ipc_manager.cpp
    )

    target_include_directories(lcl-desktop-shell PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )

    target_link_libraries(lcl-desktop-shell PRIVATE
        lcl-ui
        lcl-raster
        lcl-protocol
        lcl-shell-state
        lcl-session
        Threads::Threads
    )

    # Mobile shell presentation target. It shares compositor-owned state and
    # lcl-ui with desktop while keeping form-factor UI policy independent.
    add_executable(lcl-mobile-shell
        src/apps/mobile_shell/ui_main.cpp
    )

    target_include_directories(lcl-mobile-shell PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )

    target_link_libraries(lcl-mobile-shell PRIVATE
        lcl-ui
        lcl-raster
        lcl-protocol
        Threads::Threads
    )

    # Standalone Client Terminal App Target (lcl-terminal)
    add_executable(lcl-terminal
        src/tools/terminal_main.cpp
        src/apps/terminal/terminal_app.cpp
        src/apps/terminal/terminal_view.cpp
        src/core/terminal/pty_manager.cpp
    )

    target_include_directories(lcl-terminal PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    )

    target_link_libraries(lcl-terminal PRIVATE
        lcl-ui
        lcl-raster
        lcl-protocol
        Threads::Threads
    )

    # JavaScript Runtime Executable Target (lcl-js)
    add_executable(lcl-js
        src/tools/js_main.cpp
        src/binding/js_runtime.cpp
    )

    target_include_directories(lcl-js PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
        ${quickjs_SOURCE_DIR}
    )

    target_link_libraries(lcl-js PRIVATE
        lcl-ui
        lcl-raster
        qjs
        Threads::Threads
    )

    # Subdirectories
    add_subdirectory(apps/ui_demo)
    add_subdirectory(apps/ui_demo_js)

    # Unit Testing Framework Configuration
    option(BUILD_TESTS "Build LCL OS unit tests" ON)

    if(BUILD_TESTS)
        enable_testing()
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)

        add_executable(lcl_unit_tests
            tests/test_platform_interfaces.cpp
            tests/test_gestalt.cpp
            tests/test_motion.cpp
            tests/test_lcl_protocol.cpp
            tests/test_app_bundle_parser.cpp
            tests/test_session_service.cpp
            tests/test_ipc_manager.cpp
            tests/test_input_manager.cpp
            tests/test_system_gesture_arena.cpp
            tests/test_windowing_policy.cpp
            tests/test_key_mapper.cpp
            tests/test_compositor_components.cpp
            src/core/shell/shell_state_client.cpp
            src/core/shell/shell_state_model.cpp
            tests/test_shell_state_model.cpp
            tests/test_window_manager.cpp
            tests/test_lcl_ui.cpp
            tests/test_lcl_ui_events.cpp
            tests/test_js_runtime.cpp
            src/core/ipc/ipc_manager.cpp
            src/core/compositor/frame_scheduler.cpp
            src/core/compositor/input_router.cpp
            src/core/compositor/surface_registry.cpp
            src/core/compositor/system_surface_policy.cpp
            src/core/compositor/windowing_policy.cpp
            src/core/scene/focus_controller.cpp
            src/core/scene/scene_registry.cpp
            src/core/scene/shell_state_broker.cpp
            src/core/app/app_bundle_parser.cpp
            src/core/input/input_manager.cpp
            src/core/input/system_gesture_arena.cpp
            src/platform/android/android_input_backend.cpp
            src/render/window_manager.cpp
            src/binding/js_runtime.cpp
        )

        add_executable(lcl_ui_boundary_smoke
            tests/lcl_ui_boundary_smoke.cpp
        )

        target_include_directories(lcl_ui_boundary_smoke PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
        )

        target_link_libraries(lcl_ui_boundary_smoke PRIVATE
            lcl-ui
            Threads::Threads
        )

        target_compile_options(lcl_unit_tests PRIVATE -frtti)
        target_compile_options(gtest PRIVATE -frtti)
        target_compile_options(gtest_main PRIVATE -frtti)

        target_include_directories(lcl_unit_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
            ${quickjs_SOURCE_DIR}
            ${LIBINPUT_INCLUDE_DIRS}
            ${UDEV_INCLUDE_DIRS}
        )

        target_link_libraries(lcl_unit_tests PRIVATE
            gtest_main
            gtest
            Threads::Threads
            lcl-ui
            lcl-window-chrome
            lcl-raster
            lcl-platform-desktop
            lcl-protocol
            lcl-session
            qjs
            ${LIBINPUT_LIBRARIES}
            ${UDEV_LIBRARIES}
        )

        include(GoogleTest)
        gtest_discover_tests(lcl_unit_tests
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME LclUiBoundarySmoke COMMAND lcl_ui_boundary_smoke)
        set_tests_properties(LclUiBoundarySmoke PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
