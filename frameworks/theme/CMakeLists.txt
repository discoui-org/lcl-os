cmake_minimum_required(VERSION 3.20)
project(lcl-theme VERSION 0.1.0 LANGUAGES CXX)

add_library(lcl-theme STATIC
    src/theme.cpp
)

target_include_directories(lcl-theme PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lcl-theme PUBLIC
    lcl-graphics
)

target_compile_features(lcl-theme PUBLIC cxx_std_20)
