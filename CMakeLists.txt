cmake_minimum_required(VERSION 3.20)
project(lcl-core VERSION 0.1.0 LANGUAGES C CXX)

# Require C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic)

# Dependencies
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)
pkg_check_modules(LIBINPUT REQUIRED libinput)
pkg_check_modules(UDEV REQUIRED libudev)

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
