add_executable(lcl-sessiond ${PROJECT_SOURCE_DIR}/system/session/main.cpp)
target_include_directories(lcl-sessiond PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-sessiond PRIVATE
    lcl-session lcl-platform-common Threads::Threads
)

add_executable(lcl-sandboxd ${PROJECT_SOURCE_DIR}/system/security/sandbox_daemon_main.cpp)
target_include_directories(lcl-sandboxd PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-sandboxd PRIVATE lcl-security lcl-session Threads::Threads)

add_executable(lcl-securityd ${PROJECT_SOURCE_DIR}/system/security/security_admin_daemon_main.cpp)
target_include_directories(lcl-securityd PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-securityd PRIVATE lcl-security Threads::Threads)

add_executable(lcl-sandbox-probe ${PROJECT_SOURCE_DIR}/system/security/sandbox_platform_probe_main.cpp)
target_include_directories(lcl-sandbox-probe PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-sandbox-probe PRIVATE lcl-security)

add_executable(lcl-shell-launcher ${PROJECT_SOURCE_DIR}/system/shells/launcher_main.cpp)
target_include_directories(lcl-shell-launcher PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-shell-launcher PRIVATE lcl-platform-common lcl-security)

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

add_executable(lcl-settings settings/main.cpp)
target_include_directories(lcl-settings PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
target_link_libraries(lcl-settings PRIVATE
    lcl-ui lcl-raster lcl-protocol lcl-security Threads::Threads
)

# A non-GUI system bundle used only to prove the end-to-end third-party
# sandbox boundary on QEMU.  It has no privileged IPC, compositor, or device
# dependency and reports its narrow checks through its exit status.
add_executable(lcl-sandbox-smoke
    sandbox_probe/main.cpp
)
target_include_directories(lcl-sandbox-smoke PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-sandbox-smoke PRIVATE Threads::Threads)

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
        lcl-sandboxd
        lcl-securityd
        lcl-sandbox-probe
        lcl-shell-launcher
        lcl-open
        lcl-desktop-shell
        lcl-mobile-shell
        lcl-terminal
        lcl-settings
        lcl-sandbox-smoke
        lcl-js)
    set_target_properties(${root_runtime_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endforeach()
