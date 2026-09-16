find_package(Vulkan REQUIRED)

# Discovery and logical-device setup only. Resource allocation, commands, and
# native-buffer export remain disabled until their broker protocol is defined.
add_library(lcl-vulkan-icd SHARED src/lcl_vulkan_icd.cpp)
target_include_directories(lcl-vulkan-icd PRIVATE ${PROJECT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
target_link_libraries(lcl-vulkan-icd PRIVATE lcl-gpu-client)
target_compile_features(lcl-vulkan-icd PRIVATE cxx_std_20)
set_target_properties(lcl-vulkan-icd PROPERTIES
    OUTPUT_NAME lcl_vulkan_icd
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

if(BUILD_TESTS)
    add_executable(lcl_vulkan_icd_tests test_lcl_vulkan_icd.cpp)
    target_include_directories(lcl_vulkan_icd_tests PRIVATE ${PROJECT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(lcl_vulkan_icd_tests PRIVATE
        LCL_VULKAN_ICD_PATH="$<TARGET_FILE:lcl-vulkan-icd>"
    )
    target_link_libraries(lcl_vulkan_icd_tests PRIVATE
        gtest_main lcl-vulkan-icd lcl-gpu-protocol Threads::Threads
    )
    set_target_properties(lcl_vulkan_icd_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
