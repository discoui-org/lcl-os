cmake_minimum_required(VERSION 3.20)
project(lcl-ui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)

include(FetchContent)
FetchContent_Declare(
    yoga
    URL https://github.com/facebook/yoga/archive/refs/tags/v2.0.0.tar.gz
)
set(YOGA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yoga)

add_library(lcl-ui STATIC
    src/core/animation.cpp
    src/core/rect.cpp
    src/core/render_pass.cpp
    src/core/event_dispatcher.cpp
    src/core/window_app.cpp
    src/layout/yoga_node.cpp
    src/widgets/widget.cpp
    src/widgets/container.cpp
    src/widgets/text.cpp
    src/widgets/button.cpp
    src/widgets/backdrop_surface.cpp
    src/widgets/filter_group.cpp
    ${CMAKE_SOURCE_DIR}/src/render/skia_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/render/font_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/display/egl_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ipc/lcl_protocol.cpp
)

target_include_directories(lcl-ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${yoga_SOURCE_DIR}
    ${DRM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
)

target_link_libraries(lcl-ui PUBLIC
    yogacore
    Threads::Threads
    ${DRM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    ${GBM_LIBRARIES}
)
