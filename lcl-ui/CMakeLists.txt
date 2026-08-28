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
FetchContent_GetProperties(yoga)
if(NOT yoga_POPULATED)
    FetchContent_Populate(yoga)
    add_subdirectory(${yoga_SOURCE_DIR}/yoga ${yoga_BINARY_DIR}/yoga EXCLUDE_FROM_ALL)
endif()

add_library(lcl-ui STATIC
    src/core/image_loader.cpp
    src/core/motion.cpp
    src/core/render_pass.cpp
    src/core/event_dispatcher.cpp
    src/core/window_app.cpp
    src/core/raster_service_client.cpp
    src/layout/yoga_node.cpp
    src/widgets/widget.cpp
    src/widgets/container.cpp
    src/widgets/text.cpp
    src/widgets/text_field.cpp
    src/widgets/image.cpp
    src/widgets/button.cpp
    src/widgets/backdrop_surface.cpp
    src/widgets/filter_group.cpp
    src/widgets/scroll_view.cpp
    src/widgets/popover.cpp
    src/widgets/menu.cpp
    src/widgets/toggle.cpp
    src/widgets/picker.cpp
    src/widgets/slider.cpp
    src/widgets/progress_view.cpp
    src/widgets/divider.cpp
    src/widgets/tab_view.cpp
    src/widgets/window_chrome.cpp
    src/core/transient_controller.cpp
)

target_include_directories(lcl-ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${yoga_SOURCE_DIR}
)

target_link_libraries(lcl-ui PUBLIC
    lcl-platform-common
    lcl-motion
    lcl-graphics
    lcl-theme
    lcl-window-chrome
    lcl-protocol
    lcl-raster-protocol
    lcl-font-metrics
    yogacore
    Threads::Threads
)
