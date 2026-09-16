add_library(lcl-gpu-client STATIC
    src/gpu_client.cpp
    src/gfxstream_packet_stream.cpp
)

# Vulkan command execution always remains in the selected platform ICD.  This
# layer only creates exportable presentation targets and translates them into
# the common SurfaceClient contract. Android-native binaries do not embed the
# rootfs Vulkan presentation bridge.
if(NOT ANDROID)
    find_package(Vulkan REQUIRED)
    target_sources(lcl-gpu-client PRIVATE
        src/gl_presentation.cpp
        src/vulkan_presentation.cpp
    )
    target_include_directories(lcl-gpu-client PRIVATE ${GLES2_INCLUDE_DIRS})
    target_link_libraries(lcl-gpu-client PUBLIC Vulkan::Vulkan)
    target_link_libraries(lcl-gpu-client PRIVATE ${GLES2_LIBRARIES})
    if(LCL_BUILD_ANDROID_GFXSTREAM_GUEST_TRANSPORT)
        target_compile_definitions(lcl-gpu-client PRIVATE
            LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION=1
        )
        target_include_directories(lcl-gpu-client PRIVATE
            ${PROJECT_SOURCE_DIR}/third_party/gfxstream/common/vulkan/include
        )
    endif()
endif()

target_include_directories(lcl-gpu-client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/frameworks/client/include
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(lcl-gpu-client PUBLIC
    lcl-gpu-protocol
    lcl-client
)

target_compile_features(lcl-gpu-client PUBLIC cxx_std_20)
set_target_properties(lcl-gpu-client PROPERTIES POSITION_INDEPENDENT_CODE ON)
