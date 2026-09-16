# AEMU is an emulator-host dependency used only by LCL's optional Android
# gfxstream backend.  Do not include this file from a common or Linux path.
function(lcl_add_android_aemu)
    set(AEMU_COMMON_BUILD_CONFIG gfxstream CACHE STRING
        "AEMU profile used by the Android gfxstream backend" FORCE)
    set(BUILD_STANDALONE ON CACHE BOOL
        "Build the self-contained AEMU libraries for the Android gfxstream backend" FORCE)

    add_subdirectory(
        "${PROJECT_SOURCE_DIR}/third_party/aemu"
        "${CMAKE_BINARY_DIR}/third_party/aemu"
        EXCLUDE_FROM_ALL
    )

    # AEMU's CMake build adds host-common/include/host-common as an ordinary
    # -I path for its implementation files.  On Android NDK that shadows
    # libc++'s system <features.h> with AEMU's unrelated features.h.  Soong
    # treats it as a local quote-only include.  Match that behavior here
    # without patching the pinned upstream source tree.
    set(_lcl_aemu_local_headers
        "${PROJECT_SOURCE_DIR}/third_party/aemu/host-common/include/host-common")
    # The optional pkg-config install profile calls this target aemu-logging;
    # the embedded profile calls it logging-base.
    foreach(_lcl_aemu_target IN ITEMS logging-base aemu-logging aemu-host-common)
        if(NOT TARGET ${_lcl_aemu_target})
            continue()
        endif()
        get_target_property(_lcl_aemu_includes ${_lcl_aemu_target} INCLUDE_DIRECTORIES)
        if(_lcl_aemu_includes)
            list(REMOVE_ITEM _lcl_aemu_includes "${_lcl_aemu_local_headers}")
            set_property(TARGET ${_lcl_aemu_target} PROPERTY
                         INCLUDE_DIRECTORIES "${_lcl_aemu_includes}")
        endif()
        target_compile_options(${_lcl_aemu_target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-iquote${_lcl_aemu_local_headers}>")
    endforeach()
endfunction()
