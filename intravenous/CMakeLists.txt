cmake_minimum_required(VERSION 3.20)

project(intravenous LANGUAGES CXX)

add_executable(intravenous
    main.cpp
)

target_compile_features(intravenous PRIVATE cxx_std_23)
set_target_properties(intravenous PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(intravenous SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/random123
)

if(MSVC)
    target_compile_options(intravenous PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous PRIVATE -Wall -Wextra -Wpedantic)
endif()
