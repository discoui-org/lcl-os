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

# Builds the pinned gfxstream host only for the opt-in Android gpud backend.
# The upstream source checkout stays clean: a hash-addressed detached worktree
# receives LCL's small headless-host patch before it is added to this build.
function(lcl_add_android_gfxstream_host)
    if(NOT ANDROID)
        message(FATAL_ERROR "The LCL gfxstream host is Android-only")
    endif()
    if(LCL_ANDROID_HIDL_ROOT AND
       EXISTS "${LCL_ANDROID_HIDL_ROOT}/hidl-config.cmake")
        include("${LCL_ANDROID_HIDL_ROOT}/hidl-config.cmake")
    endif()

    set(_lcl_gfxstream_source "${PROJECT_SOURCE_DIR}/third_party/gfxstream")
    set(_lcl_gfxstream_pin "6247388dea0ce60c1dcb9cfc3572c05e0e912697")
    set(_lcl_gfxstream_patch
        "${PROJECT_SOURCE_DIR}/frameworks/gfxstream/patches/0002-lcl-headless-host.patch")
    if(NOT EXISTS "${_lcl_gfxstream_patch}")
        message(FATAL_ERROR "Missing LCL gfxstream headless-host patch")
    endif()
    file(SHA256 "${_lcl_gfxstream_patch}" _lcl_gfxstream_patch_hash)
    string(SUBSTRING "${_lcl_gfxstream_patch_hash}" 0 16 _lcl_gfxstream_patch_key)
    set(_lcl_gfxstream_overlay
        "${CMAKE_BINARY_DIR}/third_party/gfxstream-host-${_lcl_gfxstream_patch_key}")
    if(NOT EXISTS "${_lcl_gfxstream_overlay}/CMakeLists.txt")
        execute_process(
            COMMAND git -C "${_lcl_gfxstream_source}" worktree add --detach
                    "${_lcl_gfxstream_overlay}" "${_lcl_gfxstream_pin}"
            RESULT_VARIABLE _lcl_gfxstream_worktree_status
            OUTPUT_VARIABLE _lcl_gfxstream_worktree_output
            ERROR_VARIABLE _lcl_gfxstream_worktree_error)
        if(NOT _lcl_gfxstream_worktree_status EQUAL 0)
            message(FATAL_ERROR "Could not create pinned gfxstream host overlay: "
                "${_lcl_gfxstream_worktree_output}${_lcl_gfxstream_worktree_error}")
        endif()
    endif()
    set(_lcl_gfxstream_patch_marker
        "${_lcl_gfxstream_overlay}/.lcl-headless-host-${_lcl_gfxstream_patch_key}")
    if(NOT EXISTS "${_lcl_gfxstream_patch_marker}")
        execute_process(
            COMMAND git -C "${_lcl_gfxstream_overlay}" apply --check "${_lcl_gfxstream_patch}"
            RESULT_VARIABLE _lcl_gfxstream_check_status
            ERROR_VARIABLE _lcl_gfxstream_check_error)
        if(NOT _lcl_gfxstream_check_status EQUAL 0)
            message(FATAL_ERROR "LCL gfxstream host patch does not apply: ${_lcl_gfxstream_check_error}")
        endif()
        execute_process(
            COMMAND git -C "${_lcl_gfxstream_overlay}" apply "${_lcl_gfxstream_patch}"
            RESULT_VARIABLE _lcl_gfxstream_apply_status
            ERROR_VARIABLE _lcl_gfxstream_apply_error)
        if(NOT _lcl_gfxstream_apply_status EQUAL 0)
            message(FATAL_ERROR "Could not patch LCL gfxstream host overlay: ${_lcl_gfxstream_apply_error}")
        endif()
        file(WRITE "${_lcl_gfxstream_patch_marker}" "${_lcl_gfxstream_patch_hash}\n")
    endif()

    set(LCL_GFXSTREAM_HEADLESS_HOST ON CACHE BOOL
        "Build the LCL Android Vulkan-only headless gfxstream host" FORCE)
    set(BUILD_STANDALONE ON CACHE BOOL
        "Reuse LCL's embedded AEMU targets for the gfxstream host" FORCE)
    add_subdirectory("${_lcl_gfxstream_overlay}"
        "${CMAKE_BINARY_DIR}/third_party/gfxstream-host-build" EXCLUDE_FROM_ALL)
    # VkAndroidNativeBuffer is part of the upstream Vulkan decoder and uses
    # Android's native_handle declaration.  Supply the already prepared VNDK
    # headers only to the Android host targets; they never reach rootfs code.
    if(LCL_ANDROID_HIDL_INCLUDE_DIRS)
        foreach(_lcl_gfxstream_target IN ITEMS
                OpenglRender_vulkan_cereal emulated_textures gfxstream-vulkan-server
                gfxstream_backend_static gfxstream_backend)
            if(TARGET ${_lcl_gfxstream_target})
                target_include_directories(${_lcl_gfxstream_target} PRIVATE
                    ${LCL_ANDROID_HIDL_INCLUDE_DIRS})
            endif()
        endforeach()
    endif()
endfunction()
