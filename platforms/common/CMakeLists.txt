add_library(lcl-platform-common STATIC
    gestalt.cpp
    keyboard_mapper.cpp
    output_scale.cpp
)

target_include_directories(lcl-platform-common PUBLIC
    ${PROJECT_SOURCE_DIR}
)
