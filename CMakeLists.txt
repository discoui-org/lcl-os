cmake_minimum_required(VERSION 3.20)
project(lcl-core VERSION 0.1.0 LANGUAGES C CXX)

# Require C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(DRM libdrm)
pkg_check_modules(GBM gbm)
pkg_check_modules(EGL egl)
pkg_check_modules(GLES2 glesv2)
pkg_check_modules(LIBINPUT libinput)
pkg_check_modules(UDEV libudev)

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

# Shared compositor/client IPC wire-format implementation.
add_library(lcl-protocol STATIC
    src/core/ipc/lcl_protocol.cpp
)

target_include_directories(lcl-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Display scale is shared policy with no renderer or platform-GPU dependency.
# Keeping it independent lets lcl-ui consume logical-pixel configuration
# without pulling compositor display code into application binaries.
add_library(lcl-display-scale STATIC
    src/core/display/display_scale.cpp
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
)

target_include_directories(lcl-shell-state PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lcl-shell-state PUBLIC
    lcl-protocol
)

target_include_directories(lcl-display-scale PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Backend-neutral widgets, layout, events and WindowApp lifecycle.
add_subdirectory(lcl-ui)

# Client SHM raster backend. The same raster implementation remains in use,
# but its OpenGL/EGL branches are compiled out so applications do not acquire
# compositor GPU or DRM dependencies.
add_library(lcl-canvas-skia STATIC
    src/render/skia_canvas.cpp
    src/render/skia_renderer.cpp
    src/render/font_renderer.cpp
)

target_compile_definitions(lcl-canvas-skia PRIVATE LCL_SOFTWARE_ONLY=1)

target_include_directories(lcl-canvas-skia PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
)

target_link_libraries(lcl-canvas-skia PUBLIC
    lcl-ui
    lcl-protocol
    Threads::Threads
)

# Compositor-owned render/present layer. EGL/DRM/GBM/GLES stay here and never
# flow through lcl-ui or the client Canvas target.
add_library(lcl-render STATIC
    src/core/compositor/compositor_renderer.cpp
    src/core/display/display_manager.cpp
    src/core/display/egl_backend.cpp
    src/render/font_renderer.cpp
    src/render/renderer.cpp
    src/render/skia_canvas.cpp
    src/render/skia_renderer.cpp
    src/render/window_manager.cpp
)

target_include_directories(lcl-render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
)

target_link_libraries(lcl-render PUBLIC
    lcl-ui
    lcl-protocol
    lcl-display-scale
    ${DRM_LIBRARIES}
    ${GBM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    Threads::Threads
)

target_compile_options(lcl-render PRIVATE
    ${DRM_CFLAGS_OTHER}
    ${GBM_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES2_CFLAGS_OTHER}
)

# Main Compositor Executable Target
add_executable(lcl-core
    src/main.cpp
    src/core/compositor/compositor.cpp
    src/core/compositor/frame_scheduler.cpp
    src/core/compositor/input_router.cpp
    src/core/compositor/protocol_dispatcher.cpp
    src/core/compositor/surface_registry.cpp
    src/core/scene/focus_controller.cpp
    src/core/scene/scene_registry.cpp
    src/core/scene/shell_state_broker.cpp
    src/core/input/input_manager.cpp
    src/core/input/key_mapper.cpp
    src/core/ipc/ipc_manager.cpp
)

target_include_directories(lcl-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
)

target_link_libraries(lcl-core PRIVATE
    lcl-render
    lcl-protocol
    ${LIBINPUT_LIBRARIES}
    ${UDEV_LIBRARIES}
    Threads::Threads
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

# Standalone Window Manager Daemon Target (lcl-desktop-wm)
add_executable(lcl-desktop-wm
    src/tools/desktop_wm_main.cpp
    src/core/ipc/ipc_manager.cpp
)

target_include_directories(lcl-desktop-wm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lcl-desktop-wm PRIVATE
    lcl-protocol
    Threads::Threads
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
    lcl-canvas-skia
    lcl-protocol
    lcl-shell-state
    lcl-session
    Threads::Threads
)

# Standalone Client Terminal App Target (lcl-terminal)
add_executable(lcl-terminal
    src/tools/terminal_main.cpp
    src/apps/terminal/terminal_app.cpp
    src/apps/terminal/terminal_view.cpp
    src/core/input/key_mapper.cpp
    src/core/terminal/pty_manager.cpp
)

target_include_directories(lcl-terminal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
)

target_link_libraries(lcl-terminal PRIVATE
    lcl-ui
    lcl-canvas-skia
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
    lcl-canvas-skia
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
        tests/test_lcl_protocol.cpp
        tests/test_app_bundle_parser.cpp
        tests/test_session_service.cpp
        tests/test_display_scale.cpp
        tests/test_ipc_manager.cpp
        tests/test_input_manager.cpp
        tests/test_key_mapper.cpp
        tests/test_compositor_components.cpp
        src/core/shell/shell_state_client.cpp
        tests/test_window_manager.cpp
        tests/test_lcl_ui.cpp
        tests/test_lcl_ui_events.cpp
        tests/test_js_runtime.cpp
        src/core/ipc/ipc_manager.cpp
        src/core/compositor/frame_scheduler.cpp
        src/core/compositor/surface_registry.cpp
        src/core/scene/focus_controller.cpp
        src/core/scene/scene_registry.cpp
        src/core/scene/shell_state_broker.cpp
        src/core/app/app_bundle_parser.cpp
        src/core/display/display_scale.cpp
        src/core/input/input_manager.cpp
        src/core/input/key_mapper.cpp
        src/render/window_manager.cpp
        src/binding/js_runtime.cpp
    )

    # This executable deliberately links only lcl-ui. It exercises WindowApp
    # with an injected mock Canvas so any accidental renderer/EGL/DRM
    # dependency added to the UI target fails at link time.
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
        lcl-canvas-skia
        lcl-protocol
        lcl-session
        qjs
        ${LIBINPUT_LIBRARIES}
        ${UDEV_LIBRARIES}
    )

    include(GoogleTest)
    gtest_discover_tests(lcl_unit_tests)
    add_test(NAME LclUiBoundarySmoke COMMAND lcl_ui_boundary_smoke)
endif()
