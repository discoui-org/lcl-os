cmake_minimum_required(VERSION 3.20)
project(lcl-window-chrome VERSION 0.1.0 LANGUAGES CXX)

add_library(lcl-window-chrome STATIC
    src/window_chrome.cpp
)

target_include_directories(lcl-window-chrome PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lcl-window-chrome PUBLIC
    lcl-motion
)
