add_library(lcl-motion STATIC
    src/motion.cpp
)

target_include_directories(lcl-motion PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(lcl-motion PUBLIC cxx_std_20)

