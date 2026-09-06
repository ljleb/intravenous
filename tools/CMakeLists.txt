add_library(iv_clang_source_introspection_plugin MODULE
    iv_clang_source_introspection_plugin.cpp)
set_target_properties(iv_clang_source_introspection_plugin PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    PREFIX "")
target_include_directories(iv_clang_source_introspection_plugin PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS})
target_compile_definitions(iv_clang_source_introspection_plugin PRIVATE
    ${LLVM_DEFINITIONS})
if(TARGET clang-cpp)
    target_link_libraries(iv_clang_source_introspection_plugin PRIVATE clang-cpp)
else()
    target_link_libraries(iv_clang_source_introspection_plugin PRIVATE
        clangAST
        clangBasic
        clangFrontend
        clangIndex
        clangLex
        clangSema
        clangSerialization)
endif()

target_compile_options(iv_clang_source_introspection_plugin PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(iv_module_finalize iv_module_finalize.cpp)
target_include_directories(iv_module_finalize PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LLVM_INCLUDE_DIRS})
target_compile_definitions(iv_module_finalize PRIVATE
    IV_INTERNAL_TRANSLATION_UNIT=1
    ${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(IV_MODULE_FINALIZER_LLVM_LIBS
    bitreader
    core
    executionengine
    linker
    native
    nativecodegen
    orcjit
    passes
    support
    target)
target_link_libraries(iv_module_finalize PRIVATE
    ${IV_MODULE_FINALIZER_LLVM_LIBS}
    iv_module_shared)
target_compile_options(iv_module_finalize PRIVATE
    -Wall -Wextra -Wpedantic)
