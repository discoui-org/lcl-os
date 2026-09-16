add_library(lcl-gfxstream-guest-adapter STATIC
    src/guest_socket_iostream.cpp
)

target_include_directories(lcl-gfxstream-guest-adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/gfxstream/guest/iostream/include
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(lcl-gfxstream-guest-adapter PUBLIC lcl-gpu-client)
target_compile_features(lcl-gfxstream-guest-adapter PUBLIC cxx_std_20)
set_target_properties(lcl-gfxstream-guest-adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)
