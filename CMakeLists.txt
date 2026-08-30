cmake_minimum_required(VERSION 3.20)
project(lcl-core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

include(cmake/LclSkia.cmake)
lcl_import_skia()

if(NOT ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DRM libdrm)
    pkg_check_modules(GBM gbm)
    pkg_check_modules(EGL egl)
    pkg_check_modules(GLES2 glesv2)
    pkg_check_modules(LIBINPUT libinput)
    pkg_check_modules(UDEV libudev)
endif()

if(ANDROID)
    target_link_libraries(LCL::Skia INTERFACE EGL GLESv3)
else()
    target_link_libraries(LCL::Skia INTERFACE
        ${EGL_LIBRARIES}
        ${GLES2_LIBRARIES}
    )
endif()

include(FetchContent)
FetchContent_Declare(
    quickjs
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.7.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BUILD_QJS_LIBC OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(quickjs)

foreach(quickjs_target IN ITEMS qjs qjsc qjs_exe run-test262 function_source test_conv)
    if(TARGET ${quickjs_target})
        target_compile_options(${quickjs_target} PRIVATE -Wno-error)
    endif()
endforeach()

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(frameworks)
add_subdirectory(platforms/common)
add_subdirectory(system)
add_subdirectory(frameworks/ui)

if(ANDROID)
    add_subdirectory(platforms/android)
else()
    add_subdirectory(platforms/linux)
endif()

add_subdirectory(apps)

option(BUILD_TESTS "Build LCL OS unit tests" ON)
if(ANDROID)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
