add_library(lcl-gpu-client STATIC
    src/gpu_client.cpp
    src/gfxstream_packet_stream.cpp
)

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
