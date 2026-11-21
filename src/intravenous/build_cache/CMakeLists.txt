cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(intravenous-cache LANGUAGES CXX)
    find_package(nlohmann_json 3.11 CONFIG REQUIRED)
endif()

add_library(intravenous_build_cache STATIC cache.cpp)
target_compile_features(intravenous_build_cache PUBLIC cxx_std_20)
target_link_libraries(intravenous_build_cache PRIVATE nlohmann_json::nlohmann_json)

add_executable(ivctl cli.cpp)
target_link_libraries(ivctl PRIVATE intravenous_build_cache)
install(TARGETS ivctl RUNTIME DESTINATION bin)

add_executable(iv-dev-cache development.cpp)
target_link_libraries(iv-dev-cache PRIVATE intravenous_build_cache)
