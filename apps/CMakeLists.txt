add_executable(lcl-sessiond ${PROJECT_SOURCE_DIR}/system/session/main.cpp)
target_include_directories(lcl-sessiond PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-sessiond PRIVATE
    lcl-session lcl-platform-common Threads::Threads
)

add_executable(lcl-shell-launcher ${PROJECT_SOURCE_DIR}/system/shells/launcher_main.cpp)
target_include_directories(lcl-shell-launcher PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-shell-launcher PRIVATE lcl-platform-common)

add_executable(lcl-open ${PROJECT_SOURCE_DIR}/system/session/open_main.cpp)
target_include_directories(lcl-open PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-open PRIVATE lcl-session)

add_executable(lcl-desktop-shell
    ${PROJECT_SOURCE_DIR}/system/shells/desktop/ui_main.cpp
    ${PROJECT_SOURCE_DIR}/system/ipc/ipc_manager.cpp
)
target_include_directories(lcl-desktop-shell PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
target_link_libraries(lcl-desktop-shell PRIVATE
    lcl-ui lcl-raster lcl-protocol lcl-shell-state lcl-session Threads::Threads
)

add_executable(lcl-mobile-shell ${PROJECT_SOURCE_DIR}/system/shells/mobile/ui_main.cpp)
target_include_directories(lcl-mobile-shell PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
target_link_libraries(lcl-mobile-shell PRIVATE
    lcl-ui lcl-raster lcl-protocol lcl-shell-state lcl-session Threads::Threads
)

add_executable(lcl-terminal
    terminal/main.cpp
    terminal/terminal_app.cpp
    terminal/terminal_view.cpp
    terminal/pty_manager.cpp
)
target_include_directories(lcl-terminal PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
target_link_libraries(lcl-terminal PRIVATE
    lcl-ui lcl-raster lcl-protocol Threads::Threads
)

add_executable(lcl-js
    ${PROJECT_SOURCE_DIR}/system/javascript/main.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_runtime.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_binding_support.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_animation_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_widget_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_control_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_window_app_bindings.cpp
)
target_include_directories(lcl-js PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
    ${quickjs_SOURCE_DIR}
)
target_link_libraries(lcl-js PRIVATE lcl-ui lcl-raster qjs Threads::Threads)

foreach(root_runtime_target IN ITEMS
        lcl-sessiond
        lcl-shell-launcher
        lcl-open
        lcl-desktop-shell
        lcl-mobile-shell
        lcl-terminal
        lcl-js)
    set_target_properties(${root_runtime_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endforeach()

add_subdirectory(ui_demo)
add_subdirectory(ui_demo_js)
