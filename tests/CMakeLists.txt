enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(lcl_unit_tests
    platforms/test_platform_interfaces.cpp
    platforms/test_gestalt.cpp
    frameworks/motion/test_motion.cpp
    system/test_lcl_protocol.cpp
    system/test_raster_protocol.cpp
    integration/test_raster_service_integration.cpp
    system/test_app_bundle_parser.cpp
    system/test_app_data_store.cpp
    system/test_app_identity_registry.cpp
    system/test_app_launch_registry.cpp
    system/test_application_peer_authenticator.cpp
    system/test_bundle_record.cpp
    system/test_bundle_snapshot_store.cpp
    system/test_pending_bundle_approval_store.cpp
    system/test_security_admin_protocol.cpp
    system/test_bundle_launch_gate.cpp
    system/test_bundle_signature_verifier.cpp
    system/test_permission_policy.cpp
    system/test_permission_store.cpp
    system/test_portal_elevation_authority.cpp
    system/test_session_user.cpp
    system/test_sandbox_contract.cpp
    system/test_sandbox_child_launcher.cpp
    system/test_sandbox_daemon.cpp
    system/test_sandbox_filesystem_sources.cpp
    system/test_sandbox_platform_probe.cpp
    system/test_session_service.cpp
    system/test_ipc_manager.cpp
    system/test_input_manager.cpp
    system/test_system_gesture_arena.cpp
    system/test_windowing_policy.cpp
    platforms/test_key_mapper.cpp
    system/test_compositor_components.cpp
    system/test_shell_state_model.cpp
    system/test_window_manager.cpp
    frameworks/graphics/test_display_list_wire.cpp
    frameworks/ui/test_lcl_ui.cpp
    frameworks/ui/test_retained_render_tree.cpp
    frameworks/ui/test_lcl_ui_events.cpp
    system/test_js_runtime.cpp
    ${PROJECT_SOURCE_DIR}/system/shells/state/shell_state_client.cpp
    ${PROJECT_SOURCE_DIR}/system/shells/state/shell_state_model.cpp
    ${PROJECT_SOURCE_DIR}/system/ipc/ipc_manager.cpp
    ${PROJECT_SOURCE_DIR}/system/compositor/frame_scheduler.cpp
    ${PROJECT_SOURCE_DIR}/system/compositor/input_router.cpp
    ${PROJECT_SOURCE_DIR}/system/compositor/layer_feedback_handler.cpp
    ${PROJECT_SOURCE_DIR}/system/compositor/surface_registry.cpp
    ${PROJECT_SOURCE_DIR}/system/compositor/surface_transaction_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/system/compositor/system_surface_policy.cpp
    ${PROJECT_SOURCE_DIR}/system/compositor/windowing_policy.cpp
    ${PROJECT_SOURCE_DIR}/system/scene/focus_controller.cpp
    ${PROJECT_SOURCE_DIR}/system/scene/scene_registry.cpp
    ${PROJECT_SOURCE_DIR}/system/scene/shell_state_broker.cpp
    ${PROJECT_SOURCE_DIR}/system/session/app_bundle_parser.cpp
    ${PROJECT_SOURCE_DIR}/system/input/input_manager.cpp
    ${PROJECT_SOURCE_DIR}/system/input/system_gesture_arena.cpp
    ${PROJECT_SOURCE_DIR}/platforms/android/android_input_backend.cpp
    ${PROJECT_SOURCE_DIR}/system/render/window_manager.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_runtime.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_binding_support.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_animation_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_widget_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_control_bindings.cpp
    ${PROJECT_SOURCE_DIR}/system/javascript/js_window_app_bindings.cpp
)

add_executable(lcl_ui_boundary_smoke frameworks/ui/lcl_ui_boundary_smoke.cpp)
target_include_directories(lcl_ui_boundary_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
)
target_link_libraries(lcl_ui_boundary_smoke PRIVATE lcl-ui Threads::Threads)
set_target_properties(lcl_unit_tests lcl_ui_boundary_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_compile_options(lcl_unit_tests PRIVATE -frtti)
target_compile_definitions(lcl_unit_tests PRIVATE
    LCL_TEST_RASTERD_PATH="$<TARGET_FILE:lcl-rasterd>"
)
add_dependencies(lcl_unit_tests lcl-rasterd)
target_compile_options(gtest PRIVATE -frtti)
target_compile_options(gtest_main PRIVATE -frtti)
target_include_directories(lcl_unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/frameworks/ui/include
    ${PROJECT_SOURCE_DIR}/frameworks/ui/src
    ${quickjs_SOURCE_DIR}
    ${LIBINPUT_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
)
target_link_libraries(lcl_unit_tests PRIVATE
    gtest_main gtest Threads::Threads lcl-ui lcl-window-chrome lcl-raster lcl-security
    lcl-platform-desktop lcl-protocol lcl-raster-protocol lcl-session qjs
    ${LIBINPUT_LIBRARIES} ${UDEV_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(lcl_unit_tests WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME LclUiBoundarySmoke COMMAND lcl_ui_boundary_smoke)
set_tests_properties(LclUiBoundarySmoke PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME AndroidCliBuildTargets
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_main_cli.py
)
set_tests_properties(AndroidCliBuildTargets PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(
    NAME StrictAppBundleSchema
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_bundle_schema.py
)
set_tests_properties(StrictAppBundleSchema PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(
    NAME PrepareSkiaSourceRecovery
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_prepare_skia.py
)
set_tests_properties(PrepareSkiaSourceRecovery PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(
    NAME SkiaPackageRelease
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tooling/test_skia_packages.py
)
set_tests_properties(SkiaPackageRelease PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
