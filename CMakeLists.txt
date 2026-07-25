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
pkg_check_modules(LIBINPUT REQUIRED libinput)
pkg_check_modules(UDEV REQUIRED libudev)

# Executable target
add_executable(lcl-core
    src/main.cpp
    src/core/display/display_manager.cpp
    src/core/input/input_manager.cpp
    src/render/renderer.cpp
    src/render/window_manager.cpp
)

target_include_directories(lcl-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DRM_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
)

target_link_libraries(lcl-core PRIVATE
    ${DRM_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${UDEV_LIBRARIES}
    Threads::Threads
)

target_compile_options(lcl-core PRIVATE
    ${DRM_CFLAGS_OTHER}
    ${LIBINPUT_CFLAGS_OTHER}
    ${UDEV_CFLAGS_OTHER}
)
