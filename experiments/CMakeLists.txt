cmake_minimum_required(VERSION 3.30)

project(intravenous-reflection-prototype LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "The reflection prototype requires GCC 16 or newer")
endif()

enable_testing()

add_executable(intravenous_reflection_gcc16 reflection_gcc16.cpp)
target_compile_features(intravenous_reflection_gcc16 PRIVATE cxx_std_26)
target_compile_options(intravenous_reflection_gcc16 PRIVATE
    -freflection
    -Wall
    -Wextra
    -Wpedantic)

add_test(NAME intravenous_reflection_gcc16 COMMAND intravenous_reflection_gcc16)

add_test(
    NAME intravenous_reflection_gcc16_rejects_non_structural_constant
    COMMAND
        "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/reflection_gcc16_non_structural.cpp"
        "-DOBJECT=${CMAKE_CURRENT_BINARY_DIR}/reflection_gcc16_non_structural.o"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/verify_non_structural_rejection.cmake")
