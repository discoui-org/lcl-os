add_library(lcl-qt STATIC
    include/lcl-qt/quick_surface.hpp
    src/quick_surface.cpp
    src/private/frame_transport.hpp
)

# The shared Qt/QML bridge is intentionally platform-neutral.  Its first
# native frame transport is Linux DMA-BUF only; do not silently compile that
# Mesa/GBM implementation into an Android Qt build.
if(ANDROID)
    message(FATAL_ERROR
        "lcl-qt needs an Android native frame transport; Linux DMA-BUF is not valid here")
endif()
target_sources(lcl-qt PRIVATE
    src/private/linux_dma_buf_frame_transport.cpp
    ${PROJECT_SOURCE_DIR}/system/render/client_egl_context.cpp
)

set_target_properties(lcl-qt PROPERTIES AUTOMOC ON)

target_include_directories(lcl-qt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(lcl-qt PUBLIC
    lcl-client
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)

target_include_directories(lcl-qt PRIVATE
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
)
target_link_libraries(lcl-qt PRIVATE
    ${DRM_LIBRARIES}
    ${GBM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
)
target_compile_options(lcl-qt PRIVATE
    ${DRM_CFLAGS_OTHER}
    ${GBM_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES2_CFLAGS_OTHER}
)

target_compile_features(lcl-qt PUBLIC cxx_std_20)
