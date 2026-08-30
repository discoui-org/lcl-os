set(LCL_ANDROID_HIDL_ROOT "" CACHE PATH
    "Prepared Android HIDL dependency root from tooling/build/prepare_android_hidl.py")
if(LCL_ANDROID_HIDL_ROOT)
    include("${LCL_ANDROID_HIDL_ROOT}/hidl-config.cmake")
endif()

file(GLOB_RECURSE ANDROID_AIDL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/aidl/src/*.cpp"
)

add_library(lcl-platform-android STATIC
    binder_ndk_compat.cpp
    android_input_backend.cpp
    android_display_backend.cpp
    android_hidl_display_backend.cpp
    android_graphics_context.cpp
    android_platform_services.cpp
    ${ANDROID_AIDL_SOURCES}
)
target_include_directories(lcl-platform-android BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/binder_ndk_compat
)
target_include_directories(lcl-platform-android PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/aidl/include
)
target_compile_definitions(lcl-platform-android PRIVATE BINDER_STABILITY_SUPPORT=1)
target_link_libraries(lcl-platform-android PUBLIC
    lcl-platform-common binder_ndk EGL GLESv3 android dl
)

if(LCL_ANDROID_HIDL_ROOT)
    add_library(lcl-android-hidl-bridge SHARED android_hidl_bridge.cpp)
    target_compile_definitions(lcl-android-hidl-bridge PRIVATE LCL_HAS_ANDROID_HIDL=1)
    target_include_directories(lcl-android-hidl-bridge BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/hidl_libcpp_overlay
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/hidl/include
        ${PROJECT_SOURCE_DIR}
        ${LCL_ANDROID_HIDL_INCLUDE_DIRS}
    )
    target_compile_options(lcl-android-hidl-bridge PRIVATE
        -fno-rtti -Wno-deprecated-declarations
        -Wno-deprecated-copy-with-user-provided-copy
        -Wno-inconsistent-missing-override -Wno-format-pedantic
        -Wno-switch
    )
    target_link_options(lcl-android-hidl-bridge PRIVATE -nostdlib++)
    target_link_libraries(lcl-android-hidl-bridge PRIVATE
        ${LCL_ANDROID_HIDL_LIBRARIES} android dl
    )
    set_target_properties(lcl-android-hidl-bridge PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_dependencies(lcl-platform-android lcl-android-hidl-bridge)
endif()

add_executable(test_android_platform_services
    ${PROJECT_SOURCE_DIR}/tests/android/test_android_platform_services.cpp
)
target_include_directories(test_android_platform_services PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/aidl/include
)
target_link_libraries(test_android_platform_services PRIVATE lcl-platform-android)
set_target_properties(test_android_platform_services PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(test_android_client_raster
    ${PROJECT_SOURCE_DIR}/tests/android/test_android_client_raster.cpp
)
target_include_directories(test_android_client_raster PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_android_client_raster PRIVATE lcl-raster)
set_target_properties(test_android_client_raster PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(test_android_window_client
    ${PROJECT_SOURCE_DIR}/tests/android/test_android_window_client.cpp
)
target_include_directories(test_android_window_client PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
target_link_libraries(test_android_window_client PRIVATE
    lcl-ui lcl-raster lcl-protocol Threads::Threads
)
set_target_properties(test_android_window_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(lcl-core-android main_android.cpp)
target_include_directories(lcl-core-android PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/aidl/include
)
target_link_libraries(lcl-core-android PRIVATE lcl-core-engine lcl-platform-android)
set_target_properties(lcl-core-android PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
