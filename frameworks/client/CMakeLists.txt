add_library(lcl-client STATIC
    src/compositor_connection.cpp
    src/owned_fd.cpp
    src/raster_connection.cpp
    src/surface_client.cpp
)

target_include_directories(lcl-client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(lcl-client PUBLIC
    lcl-protocol
    lcl-raster-protocol
)

target_compile_features(lcl-client PUBLIC cxx_std_20)
