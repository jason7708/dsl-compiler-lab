cmake_minimum_required(VERSION 3.20)
project(dsl_runtime LANGUAGES CXX)

add_library(dsl_runtime STATIC src/math.cpp)
add_library(dsl::runtime ALIAS dsl_runtime)
target_include_directories(dsl_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(dsl_runtime PUBLIC cxx_std_23)
set_target_properties(dsl_runtime PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_compile_options(dsl_runtime PRIVATE -Wall -Wextra -Wpedantic -fno-fast-math -ffp-contract=off)
