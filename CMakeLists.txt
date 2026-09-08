cmake_minimum_required(VERSION 3.20)
project(dsl_compiler VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
    #include <array>
    #include <expected>
    #include <format>
    #include <print>
    #include <ranges>
    #include <string>
    int main() {
        const std::array values{1};
        std::expected<int, std::string> result = 2;
        for (auto [id, value] : values | std::views::enumerate)
            std::println("{}", std::format("{}: {}", id, value + *result));
    }
]=] DSL_HAS_CXX23_LIBRARY)
if(NOT DSL_HAS_CXX23_LIBRARY)
    message(FATAL_ERROR
        "A C++23 standard library with expected, format, print and views::enumerate is required. "
        "Select a matching compiler/standard library; this project was tested with GCC/libstdc++ 16.1.0.")
endif()

option(DSL_BUILD_COMPILER "Build the Clang DSL frontend and CLI" ON)

# Public IR and the C++ backend do not link or include LLVM/Clang.
add_library(dsl_ir STATIC src/ir.cpp src/registry.cpp src/verify.cpp)
target_include_directories(dsl_ir PUBLIC include)
target_compile_options(dsl_ir PRIVATE -Wall -Wextra -Wpedantic)
add_library(dsl_cpp_backend STATIC src/codegen.cpp)
target_link_libraries(dsl_cpp_backend PUBLIC dsl_ir)
target_compile_options(dsl_cpp_backend PRIVATE -Wall -Wextra -Wpedantic)

if(DSL_BUILD_COMPILER)
    # Keep the AST API and the linked ABI on one reproducible release.
    set(DSL_LLVM_VERSION "20.1.8")
    find_package(LLVM ${DSL_LLVM_VERSION} EXACT REQUIRED CONFIG
        HINTS /usr/lib/llvm-20/lib/cmake/llvm)
    find_path(DSL_CLANG_INCLUDE_DIR clang/Tooling/Tooling.h
        HINTS ${LLVM_INCLUDE_DIRS} REQUIRED)
    find_library(DSL_CLANG_CPP_LIBRARY
        NAMES clang-cpp libclang-cpp.so.20.1
        HINTS ${LLVM_LIBRARY_DIRS} REQUIRED)
    file(STRINGS "${DSL_CLANG_INCLUDE_DIR}/clang/Basic/Version.inc" clang_version_line
        REGEX "^#define CLANG_VERSION_STRING ")
    if(NOT clang_version_line STREQUAL "#define CLANG_VERSION_STRING \"${DSL_LLVM_VERSION}\"")
        message(FATAL_ERROR "Clang headers must be ${DSL_LLVM_VERSION}: ${clang_version_line}")
    endif()
    message(STATUS "DSL LLVM/Clang version: ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Clang headers: ${DSL_CLANG_INCLUDE_DIR}")
    message(STATUS "Clang library: ${DSL_CLANG_CPP_LIBRARY}")

    add_subdirectory(runtime)
    configure_file(cmake/build-info.txt.in build-info.txt @ONLY)
    add_library(dsl_frontend STATIC src/frontend.cpp src/frontend_ops.cpp
        src/frontend_bindings.cpp src/semantic.cpp)
    target_link_libraries(dsl_frontend PUBLIC dsl_ir PRIVATE ${DSL_CLANG_CPP_LIBRARY} LLVM)
    add_executable(dslc src/main.cpp src/config.cpp)
    target_link_libraries(dslc PRIVATE dsl_frontend dsl_cpp_backend LLVM)
    foreach(target dsl_frontend dslc)
        target_include_directories(${target} PRIVATE include)
        target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${DSL_CLANG_INCLUDE_DIR})
        target_compile_definitions(${target} PRIVATE DSL_LLVM_VERSION="${DSL_LLVM_VERSION}"
            DSL_RUNTIME_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(ir_verifier_tests tests/ir_verifier.cpp)
    target_link_libraries(ir_verifier_tests PRIVATE dsl_ir)
    add_test(NAME ir_verifier COMMAND ir_verifier_tests)
    add_executable(backend_tests tests/backend.cpp)
    target_link_libraries(backend_tests PRIVATE dsl_cpp_backend)
    add_test(NAME backend_boundary COMMAND backend_tests)
    if(DSL_BUILD_COMPILER)
        add_executable(frontend_location_tests tests/frontend_locations.cpp)
        target_link_libraries(frontend_location_tests PRIVATE dsl_frontend)
        add_test(NAME frontend_locations COMMAND frontend_location_tests)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME compiler_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                --dslc $<TARGET_FILE:dslc> --cxx ${CMAKE_CXX_COMPILER}
                --examples ${CMAKE_CURRENT_SOURCE_DIR}/examples
                --runtime-include ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include
                --runtime-library $<TARGET_FILE:dsl_runtime>)
        add_test(NAME object_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/objects.py
                --examples ${CMAKE_CURRENT_SOURCE_DIR}/examples
                --dslc $<TARGET_FILE:dslc> --cxx ${CMAKE_CXX_COMPILER}
                --runtime-include ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include
                --runtime-library $<TARGET_FILE:dsl_runtime>)
    endif()
endif()
