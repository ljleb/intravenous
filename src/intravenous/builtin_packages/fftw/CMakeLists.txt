cmake_minimum_required(VERSION 3.21)

project(iv_fftw_package LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# FFTW belongs exclusively to this optional IV package. Configuring this
# package downloads the official consumer release into the package build tree;
# neither the Intravenous application nor its default package links FFTW.
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Build FFTW tests" FORCE)
set(ENABLE_FLOAT ON CACHE BOOL "Build single-precision FFTW" FORCE)
set(ENABLE_LONG_DOUBLE OFF CACHE BOOL "Build long-double FFTW" FORCE)
set(ENABLE_QUAD_PRECISION OFF CACHE BOOL "Build quad-precision FFTW" FORCE)
set(ENABLE_OPENMP OFF CACHE BOOL "Build FFTW OpenMP support" FORCE)
set(ENABLE_THREADS OFF CACHE BOOL "Build FFTW thread support" FORCE)
set(DISABLE_FORTRAN ON CACHE BOOL "Disable FFTW Fortran wrappers" FORCE)

FetchContent_Declare(
    fftw
    URL https://www.fftw.org/fftw-3.3.11.tar.gz
    URL_HASH SHA256=5630c24cdeb33b131612f7eb4b1a9934234754f9f388ff8617458d0be6f239a1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(fftw)

include(${IV_SOURCE_DIR}/module/template/ModuleSupport.cmake)
iv_add_package(iv_package)

# One package-owned native adapter provides the process-shared FFTW planner
# lock and owns all native plan/buffer objects. Hot-reloaded package LLVM calls
# this adapter; neither core nor iv_builder exports FFTW-specific operations.
add_library(iv_fftw_backend SHARED backend.cpp)
target_compile_features(iv_fftw_backend PRIVATE cxx_std_23)
target_compile_definitions(iv_fftw_backend PRIVATE IV_FFTW_BACKEND_BUILD=1)
target_include_directories(iv_fftw_backend PRIVATE "${IV_INCLUDE_DIR}")
target_include_directories(iv_fftw_backend SYSTEM PRIVATE "${fftw_SOURCE_DIR}/api")
target_link_libraries(iv_fftw_backend PRIVATE fftw3f)
set_target_properties(iv_fftw_backend PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# The ordinary IV-package dependency collector records this native provider and
# the loader loads it only when this package is selected and built by the user.
# List FFTW itself first as well: ELF/Mach-O can normally resolve it through the
# adapter's runtime dependency, while Windows needs the dependent DLL loaded or
# discoverable before the adapter DLL is opened.
target_link_libraries(iv_package PRIVATE iv_fftw_backend)
iv_package_add_dynamic_libraries(iv_package fftw3f iv_fftw_backend)
