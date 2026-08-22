cmake_minimum_required(VERSION 3.20)
project(lcl-graphics VERSION 0.1.0 LANGUAGES CXX)

add_library(lcl-graphics STATIC
    src/path.cpp
    src/display_list.cpp
)

target_include_directories(lcl-graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(lcl-graphics PUBLIC cxx_std_20)

