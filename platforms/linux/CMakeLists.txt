add_library(lcl-platform-desktop STATIC
    drm_display_backend.cpp
    gbm_graphics_context.cpp
    evdev_input_backend.cpp
    evdev_key_mapper.cpp
    desktop_runtime_paths.cpp
    desktop_platform_services.cpp
)
target_include_directories(lcl-platform-desktop PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${DRM_INCLUDE_DIRS} ${GBM_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS} ${LIBINPUT_INCLUDE_DIRS} ${UDEV_INCLUDE_DIRS}
)
target_link_libraries(lcl-platform-desktop PUBLIC
    lcl-platform-common ${DRM_LIBRARIES} ${GBM_LIBRARIES} ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES} ${LIBINPUT_LIBRARIES} ${UDEV_LIBRARIES}
)
target_compile_options(lcl-platform-desktop PRIVATE
    ${DRM_CFLAGS_OTHER} ${GBM_CFLAGS_OTHER} ${EGL_CFLAGS_OTHER}
    ${GLES2_CFLAGS_OTHER} ${LIBINPUT_CFLAGS_OTHER} ${UDEV_CFLAGS_OTHER}
)

add_executable(lcl-core ${PROJECT_SOURCE_DIR}/system/compositor/main.cpp)
target_include_directories(lcl-core PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lcl-core PRIVATE lcl-core-engine lcl-platform-desktop)
target_compile_options(lcl-core PRIVATE ${LIBINPUT_CFLAGS_OTHER} ${UDEV_CFLAGS_OTHER})
set_target_properties(lcl-core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
