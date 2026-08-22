cmake_minimum_required(VERSION 3.20)
project(lcl_ui_demo_js LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

# Copy main.js, Manifest.json, and Resources to build output apps/ui_demo_js/
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/apps/ui_demo_js)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/main.js DESTINATION ${CMAKE_BINARY_DIR}/apps/ui_demo_js)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/Manifest.json DESTINATION ${CMAKE_BINARY_DIR}/apps/ui_demo_js)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/Resources DESTINATION ${CMAKE_BINARY_DIR}/apps/ui_demo_js)
