cmake_minimum_required(VERSION 3.20)
project(lcl_shader_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lcl_shader_demo
    main.cpp
)

target_include_directories(lcl_shader_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lcl-ui/include
)

target_link_libraries(lcl_shader_demo PRIVATE
    lcl-ui
    Threads::Threads
)
