add_library(lcl-protocol STATIC ipc/lcl_protocol.cpp)
target_include_directories(lcl-protocol PUBLIC ${PROJECT_SOURCE_DIR})

add_library(lcl-raster-protocol STATIC ipc/raster_protocol.cpp)
target_include_directories(lcl-raster-protocol PUBLIC ${PROJECT_SOURCE_DIR})

add_library(lcl-path-raster STATIC render/path_rasterizer.cpp)
target_include_directories(lcl-path-raster PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-path-raster PUBLIC lcl-graphics)

add_library(lcl-font-metrics STATIC
    render/skia_text_engine.cpp
    render/text_metrics.cpp
)
target_include_directories(lcl-font-metrics PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
target_link_libraries(lcl-font-metrics PUBLIC lcl-graphics LCL::Skia)

add_library(lcl-session-protocol STATIC session/session_protocol.cpp)
target_include_directories(lcl-session-protocol PUBLIC ${PROJECT_SOURCE_DIR})

add_library(lcl-security STATIC
    security/app_data_store.cpp
    security/app_identity_registry.cpp
    security/bundle_approval_store.cpp
    security/bundle_launch_gate.cpp
    security/bundle_record.cpp
    security/bundle_signature_verifier.cpp
    security/desktop_user.cpp
    security/sandbox_child_launcher.cpp
    security/sandbox_child_reaper.cpp
    security/sandbox_contract.cpp
    security/sandbox_daemon.cpp
    security/sandbox_filesystem_sources.cpp
    security/sandbox_launch_authorizer.cpp
    security/sandbox_platform_probe.cpp
    security/sandbox_protocol.cpp
    security/sha256.cpp
)
target_include_directories(lcl-security PUBLIC ${PROJECT_SOURCE_DIR})

add_library(lcl-session STATIC
    session/app_bundle_parser.cpp
    session/app_registry.cpp
    session/session_service.cpp
    session/session_client.cpp
)
target_include_directories(lcl-session PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-session PUBLIC lcl-security lcl-session-protocol Threads::Threads)

add_library(lcl-shell-state STATIC
    shells/state/shell_state_client.cpp
    shells/state/shell_state_model.cpp
)
target_include_directories(lcl-shell-state PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-shell-state PUBLIC lcl-protocol)

add_library(lcl-render STATIC
    render/renderer.cpp
    render/raster_renderer.cpp
    render/window_manager.cpp
)
lcl_enable_skia_replay(lcl-render)
target_include_directories(lcl-render PUBLIC ${PROJECT_SOURCE_DIR})
if(ANDROID)
    target_link_libraries(lcl-render PUBLIC
        lcl-motion lcl-graphics lcl-theme lcl-path-raster lcl-protocol
        lcl-font-metrics lcl-platform-common GLESv3 EGL android Threads::Threads
    )
else()
    target_include_directories(lcl-render PUBLIC ${GLES2_INCLUDE_DIRS})
    target_link_libraries(lcl-render PUBLIC
        lcl-motion lcl-graphics lcl-theme lcl-path-raster lcl-protocol
        lcl-font-metrics lcl-platform-common ${GLES2_LIBRARIES} Threads::Threads
    )
    target_compile_options(lcl-render PRIVATE ${GLES2_CFLAGS_OTHER})
endif()

add_library(lcl-core-engine STATIC
    compositor/compositor.cpp
    compositor/compositor_renderer.cpp
    compositor/frame_scheduler.cpp
    compositor/input_router.cpp
    compositor/layer_feedback_handler.cpp
    compositor/protocol_dispatcher.cpp
    compositor/raster_service_host.cpp
    compositor/surface_registry.cpp
    compositor/surface_transaction_coordinator.cpp
    compositor/system_surface_policy.cpp
    compositor/windowing_policy.cpp
    scene/focus_controller.cpp
    scene/scene_registry.cpp
    scene/shell_state_broker.cpp
    input/input_manager.cpp
    input/system_gesture_arena.cpp
    ipc/ipc_manager.cpp
)
target_include_directories(lcl-core-engine PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-core-engine PUBLIC
    lcl-render lcl-platform-common lcl-protocol lcl-theme lcl-session
    lcl-motion lcl-graphics lcl-raster-protocol Threads::Threads
)

if(ANDROID)
    add_executable(lcl-rasterd-android
        render/rasterd_main.cpp
        render/client_egl_context.cpp
    )
    target_link_libraries(lcl-rasterd-android PRIVATE
        lcl-render lcl-raster-protocol lcl-graphics EGL GLESv3 android Threads::Threads
    )
    set_target_properties(lcl-rasterd-android PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
else()
    add_executable(lcl-rasterd
        render/rasterd_main.cpp
        render/client_egl_context.cpp
    )
    target_include_directories(lcl-rasterd PRIVATE
        ${PROJECT_SOURCE_DIR} ${DRM_INCLUDE_DIRS} ${GBM_INCLUDE_DIRS}
        ${EGL_INCLUDE_DIRS} ${GLES2_INCLUDE_DIRS}
    )
    target_link_libraries(lcl-rasterd PRIVATE
        lcl-render lcl-raster-protocol lcl-graphics lcl-security ${DRM_LIBRARIES}
        ${GBM_LIBRARIES} ${EGL_LIBRARIES} ${GLES2_LIBRARIES} Threads::Threads
    )
    target_compile_options(lcl-rasterd PRIVATE
        ${DRM_CFLAGS_OTHER} ${GBM_CFLAGS_OTHER} ${EGL_CFLAGS_OTHER} ${GLES2_CFLAGS_OTHER}
    )
    set_target_properties(lcl-rasterd PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

add_library(lcl-raster STATIC
    render/client_egl_context.cpp
    render/raster_canvas.cpp
    render/raster_renderer.cpp
)
lcl_enable_skia_replay(lcl-raster)
target_include_directories(lcl-raster PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
if(ANDROID)
    target_link_libraries(lcl-raster PUBLIC
        lcl-graphics lcl-path-raster lcl-font-metrics lcl-protocol
        EGL GLESv3 android Threads::Threads
    )
else()
    target_include_directories(lcl-raster PUBLIC
        ${DRM_INCLUDE_DIRS} ${GBM_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS} ${GLES2_INCLUDE_DIRS}
    )
    target_link_libraries(lcl-raster PUBLIC
        lcl-graphics lcl-path-raster lcl-font-metrics lcl-protocol
        ${DRM_LIBRARIES} ${GBM_LIBRARIES} ${EGL_LIBRARIES}
        ${GLES2_LIBRARIES} Threads::Threads
    )
    target_compile_options(lcl-raster PRIVATE
        ${DRM_CFLAGS_OTHER} ${GBM_CFLAGS_OTHER} ${EGL_CFLAGS_OTHER} ${GLES2_CFLAGS_OTHER}
    )
endif()
