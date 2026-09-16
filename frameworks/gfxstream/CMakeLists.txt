add_library(lcl-gfxstream-guest-adapter STATIC
    src/guest_socket_iostream.cpp
    src/upstream_socket_factory.cpp
)

target_include_directories(lcl-gfxstream-guest-adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/gfxstream/guest/iostream/include
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(lcl-gfxstream-guest-adapter PUBLIC lcl-gpu-client)
target_compile_features(lcl-gfxstream-guest-adapter PUBLIC cxx_std_20)
set_target_properties(lcl-gfxstream-guest-adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The decoder bridge is deliberately separate from the guest socket adapter:
# it exists only in the opt-in Android gpud build and links the pinned host
# overlay's upstream RenderChannel implementation.  No Linux or common target
# sees AEMU/gfxstream host code.
if(ANDROID AND LCL_ENABLE_ANDROID_GFXSTREAM_BACKEND)
    add_library(lcl-gfxstream-android-host-adapter STATIC
        src/android_host_ingress.cpp
    )
    target_include_directories(lcl-gfxstream-android-host-adapter PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/third_party/gfxstream/host
        ${PROJECT_SOURCE_DIR}/third_party/gfxstream/include
    )
    target_include_directories(lcl-gfxstream-android-host-adapter PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(lcl-gfxstream-android-host-adapter PRIVATE
        gfxstream_backend_static
        Threads::Threads
    )
    target_compile_definitions(lcl-gfxstream-android-host-adapter PRIVATE
        GFXSTREAM_ENABLE_HOST_GLES=0
    )
    target_compile_features(lcl-gfxstream-android-host-adapter PUBLIC cxx_std_20)
    set_target_properties(lcl-gfxstream-android-host-adapter PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endif()
