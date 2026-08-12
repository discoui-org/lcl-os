cmake_minimum_required(VERSION 3.20)
project(lcl-ui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    yoga
    URL https://github.com/facebook/yoga/archive/refs/tags/v2.0.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(YOGA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yoga)

add_library(lcl-ui STATIC
    src/core/animation.cpp
    src/core/image_loader.cpp
    src/core/rect.cpp
    src/core/render_pass.cpp
    src/core/event_dispatcher.cpp
    src/core/window_app.cpp
    src/layout/yoga_node.cpp
    src/widgets/widget.cpp
    src/widgets/container.cpp
    src/widgets/text.cpp
    src/widgets/image.cpp
    src/widgets/button.cpp
    src/widgets/backdrop_surface.cpp
    src/widgets/filter_group.cpp
)

target_include_directories(lcl-ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${yoga_SOURCE_DIR}
)

target_link_libraries(lcl-ui PUBLIC
    lcl-protocol
    lcl-display-scale
    yogacore
    Threads::Threads
)
