add_executable(intravenous_architecture_smoke
    architecture_smoke.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_architecture_smoke PRIVATE cxx_std_23)
set_target_properties(intravenous_architecture_smoke PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_architecture_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_architecture_smoke SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_architecture_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_architecture_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME intravenous_architecture_smoke COMMAND intravenous_architecture_smoke)
