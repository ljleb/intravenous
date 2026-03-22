cmake_minimum_required(VERSION 3.20)

project(intravenous-dsp LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

add_subdirectory(intravenous)
add_subdirectory(tests)
