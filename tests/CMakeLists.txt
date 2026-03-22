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

add_executable(intravenous_module_loader_variants
    module_loader_variants.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_loader_variants PRIVATE cxx_std_23)
set_target_properties(intravenous_module_loader_variants PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_loader_variants PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_loader_variants SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_loader_variants PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_loader_variants PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME intravenous_module_loader_variants COMMAND intravenous_module_loader_variants)

add_executable(intravenous_module_loader_failures
    module_loader_failures.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_loader_failures PRIVATE cxx_std_23)
set_target_properties(intravenous_module_loader_failures PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_loader_failures PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_loader_failures SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_loader_failures PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_loader_failures PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME intravenous_module_loader_failures COMMAND intravenous_module_loader_failures)

add_executable(intravenous_module_loader_reload
    module_loader_reload.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_loader_reload PRIVATE cxx_std_23)
set_target_properties(intravenous_module_loader_reload PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_loader_reload PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_loader_reload SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_loader_reload PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_loader_reload PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME intravenous_module_loader_reload COMMAND intravenous_module_loader_reload)
