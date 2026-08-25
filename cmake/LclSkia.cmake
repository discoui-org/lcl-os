set(LCL_SKIA_ROOT "" CACHE PATH
    "Target-specific Skia package containing include/ and lib/libskia.a")

function(lcl_import_skia)
    if(TARGET LCL::Skia)
        return()
    endif()
    if(NOT LCL_SKIA_ROOT)
        if(ANDROID)
            if(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
                set(skia_package_name "android-x86_64")
            elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
                set(skia_package_name "android-arm64-v8a")
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            set(skia_package_name "host-x86_64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            set(skia_package_name "host-aarch64")
        endif()
        if(skia_package_name)
            set(LCL_SKIA_ROOT
                "${PROJECT_SOURCE_DIR}/out/skia-package/${skia_package_name}")
        endif()
    endif()
    if(NOT LCL_SKIA_ROOT)
        message(FATAL_ERROR
            "No Skia package mapping for this target. Set "
            "-DLCL_SKIA_ROOT=<target-package>.")
    endif()

    cmake_path(ABSOLUTE_PATH LCL_SKIA_ROOT NORMALIZE OUTPUT_VARIABLE skia_root)
    set(skia_header "${skia_root}/include/core/SkCanvas.h")
    set(skia_library "${skia_root}/lib/libskia.a")
    if(NOT EXISTS "${skia_header}" OR NOT EXISTS "${skia_library}")
        message(FATAL_ERROR
            "Invalid LCL_SKIA_ROOT '${skia_root}': expected include/core/SkCanvas.h "
            "and lib/libskia.a")
    endif()

    add_library(LCL::Skia STATIC IMPORTED GLOBAL)
    set_target_properties(LCL::Skia PROPERTIES
        IMPORTED_LOCATION "${skia_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${skia_root}"
        INTERFACE_COMPILE_DEFINITIONS
            "SK_GANESH;SK_GL;SK_ASSUME_GL_ES=1;SK_DISABLE_TRACING"
    )

    set(skia_freetype "${skia_root}/lib/libfreetype2.a")
    if(EXISTS "${skia_freetype}")
        add_library(LCL::SkiaFreeType STATIC IMPORTED GLOBAL)
        set_target_properties(LCL::SkiaFreeType PROPERTIES
            IMPORTED_LOCATION "${skia_freetype}"
        )
        target_link_libraries(LCL::Skia INTERFACE LCL::SkiaFreeType)
    else()
        find_package(Freetype REQUIRED)
        target_link_libraries(LCL::Skia INTERFACE Freetype::Freetype)
    endif()

    # Android packages use Skia's hermetic font stack. Keep those private
    # archive dependencies beside libskia.a so no Android system font or
    # host-library ABI leaks into the shared replay implementation.
    foreach(skia_dependency IN ITEMS skcms png zlib cpu-features)
        set(dependency_library
            "${skia_root}/lib/lib${skia_dependency}.a")
        if(EXISTS "${dependency_library}")
            string(MAKE_C_IDENTIFIER "${skia_dependency}"
                dependency_target_suffix)
            set(dependency_target
                "LCL_SkiaDependency_${dependency_target_suffix}")
            add_library(${dependency_target} STATIC IMPORTED GLOBAL)
            set_target_properties(${dependency_target} PROPERTIES
                IMPORTED_LOCATION "${dependency_library}"
            )
            target_link_libraries(LCL::Skia INTERFACE ${dependency_target})
        endif()
    endforeach()

    if(ANDROID)
        # Skia's Android logging port resolves through the NDK system library.
        target_link_libraries(LCL::Skia INTERFACE log)
    endif()
endfunction()

function(lcl_enable_skia_replay target)
    target_sources(${target} PRIVATE src/render/skia_display_list_renderer.cpp)
    target_link_libraries(${target} PUBLIC LCL::Skia ${CMAKE_DL_LIBS})
endfunction()
