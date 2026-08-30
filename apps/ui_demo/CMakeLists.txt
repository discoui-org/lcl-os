cmake_minimum_required(VERSION 3.20)
project(lcl_ui_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lcl_ui_demo
    main.cpp
)

target_include_directories(lcl_ui_demo PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/frameworks/ui/include
)

target_link_libraries(lcl_ui_demo PRIVATE
    lcl-ui
    lcl-raster
    Threads::Threads
)
