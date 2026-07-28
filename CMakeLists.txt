cmake_minimum_required(VERSION 3.20)
project(lcl-core VERSION 0.1.0 LANGUAGES C CXX)

# Require C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# QuickJS Engine (C-based JavaScript Runtime)
include(FetchContent)
FetchContent_Declare(
    quickjs
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.7.0.tar.gz
)
set(BUILD_QJS_LIBC OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(quickjs)

if(TARGET qjs)
    target_compile_options(qjs PRIVATE -Wno-error -Wno-unused-parameter -Wno-format)
endif()

# Compiler flags for LCL OS core
add_compile_options(-Wall -Wextra -Wpedantic)

# Main Compositor Executable Target
add_executable(lcl-core
    src/main.cpp
    src/core/compositor/compositor.cpp
    src/apps/terminal/terminal_app.cpp
    src/core/app/app_bundle_parser.cpp
    src/core/display/display_manager.cpp
    src/core/display/display_scale.cpp
    src/core/display/egl_backend.cpp
    src/core/input/input_manager.cpp
    src/core/input/key_mapper.cpp
    src/core/ipc/ipc_manager.cpp
    src/core/ipc/lcl_protocol.cpp
    src/core/session/startup_manager.cpp
    src/core/terminal/pty_manager.cpp
    src/render/font_renderer.cpp
    src/render/renderer.cpp
    src/render/skia_renderer.cpp
    src/render/window_manager.cpp
)

target_include_directories(lcl-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
)

target_link_libraries(lcl-core PRIVATE
    ${DRM_LIBRARIES}
    ${GBM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${UDEV_LIBRARIES}
    Threads::Threads
)

target_compile_options(lcl-core PRIVATE
    ${DRM_CFLAGS_OTHER}
    ${GBM_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES2_CFLAGS_OTHER}
    ${LIBINPUT_CFLAGS_OTHER}
    ${UDEV_CFLAGS_OTHER}
)

# Native CLI App Launcher Target (lcl-open)
add_executable(lcl-open
    src/tools/open_main.cpp
    src/core/app/app_bundle_parser.cpp
    src/core/ipc/ipc_manager.cpp
    src/core/ipc/lcl_protocol.cpp
)

target_include_directories(lcl-open PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Standalone Window Manager Daemon Target (lcl-desktop-wm)
add_executable(lcl-desktop-wm
    src/tools/desktop_wm_main.cpp
    src/core/ipc/ipc_manager.cpp
    src/core/ipc/lcl_protocol.cpp
)

target_include_directories(lcl-desktop-wm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lcl-desktop-wm PRIVATE
    Threads::Threads
)

# Standalone Desktop Shell Daemon Target (lcl-desktop-shell)
add_executable(lcl-desktop-shell
    src/apps/desktop_shell/main.cpp
    src/core/ipc/ipc_manager.cpp
    src/core/ipc/lcl_protocol.cpp
    src/core/display/display_scale.cpp
    src/render/font_renderer.cpp
)

target_include_directories(lcl-desktop-shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lcl-desktop-shell PRIVATE
    Threads::Threads
)

# Standalone Client Terminal App Target (lcl-terminal)
add_executable(lcl-terminal
    src/tools/terminal_main.cpp
    src/apps/terminal/terminal_app.cpp
    src/core/input/key_mapper.cpp
    src/core/terminal/pty_manager.cpp
    src/core/display/display_scale.cpp
    src/core/ipc/ipc_manager.cpp
    src/core/ipc/lcl_protocol.cpp
    src/render/font_renderer.cpp
)

target_include_directories(lcl-terminal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lcl-ui/include
)

target_link_libraries(lcl-terminal PRIVATE
    lcl-ui
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
    qjs
    Threads::Threads
)

# Subdirectories
add_subdirectory(lcl-ui)
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
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(lcl_unit_tests
        tests/test_lcl_protocol.cpp
        tests/test_app_bundle_parser.cpp
        tests/test_display_scale.cpp
        tests/test_ipc_manager.cpp
        tests/test_input_manager.cpp
        tests/test_key_mapper.cpp
        tests/test_lcl_ui.cpp
        tests/test_lcl_ui_events.cpp
        tests/test_js_runtime.cpp
        src/core/ipc/lcl_protocol.cpp
        src/core/ipc/ipc_manager.cpp
        src/core/app/app_bundle_parser.cpp
        src/core/display/display_scale.cpp
        src/core/input/input_manager.cpp
        src/core/input/key_mapper.cpp
        src/binding/js_runtime.cpp
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
        qjs
        ${LIBINPUT_LIBRARIES}
        ${UDEV_LIBRARIES}
    )

    include(GoogleTest)
    gtest_discover_tests(lcl_unit_tests)
endif()


