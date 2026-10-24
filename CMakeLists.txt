cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Project HYPERION - x86-64 -> AArch64 user-mode dynamic binary translator.
# ---------------------------------------------------------------------------

project(hyperion
    VERSION 0.2.0
    DESCRIPTION "x86-64 to AArch64 user-mode dynamic binary translator for Linux ELF64 binaries"
    HOMEPAGE_URL "https://github.com/ransombiyato/hyperion-dbt-v2"
    LANGUAGES CXX)

# --- Global language settings ----------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Project options --------------------------------------------------------
option(HYPERION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(HYPERION_ENABLE_SANITIZERS "Build with Address/UB sanitizers" OFF)
option(HYPERION_BUILD_TESTS "Build the test suite" ON)
option(HYPERION_BUILD_BENCHMARKS "Build the benchmark harness" ON)

include(GNUInstallDirs)
include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- Compiler warnings (portable across GCC and Clang) ----------------------
add_library(hyperion_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hyperion_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wcast-qual -Wformat=2 -Wundef -Wnon-virtual-dtor)
    if(HYPERION_WARNINGS_AS_ERRORS)
        target_compile_options(hyperion_warnings INTERFACE -Werror)
    endif()
endif()

if(HYPERION_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- Core library -----------------------------------------------------------
add_library(hyperion_core
    src/core/version.cpp
    src/core/json.cpp
    src/core/host_probe.cpp
    src/core/host_syscalls.cpp
    src/core/bench_stats.cpp
    src/core/guest_state.cpp
    src/core/decoder.cpp
    src/core/aarch64_encoder.cpp
    src/core/exec_mem.cpp
    src/core/interpreter.cpp
    src/core/syscall.cpp
    src/core/syscall_dispatch.cpp
    src/core/syscall_adapters.cpp
    src/core/syscall_registry.cpp
    src/core/process.cpp
    src/core/translator.cpp
    src/core/runtime_helpers.cpp
    src/core/address_space.cpp
    src/core/elf.cpp
    src/core/elf_loader.cpp
    src/core/translation_cache.cpp)
add_library(hyperion::core ALIAS hyperion_core)

target_include_directories(hyperion_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(hyperion_core PRIVATE hyperion_warnings)

# --- CLI --------------------------------------------------------------------
add_executable(hyperion src/cli/main.cpp)
target_link_libraries(hyperion PRIVATE hyperion::core hyperion_warnings)

# --- Tests and benchmarks ---------------------------------------------------
if(HYPERION_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(HYPERION_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# --- Install rules ----------------------------------------------------------
install(TARGETS hyperion hyperion_core
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/hyperion DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

message(STATUS "HYPERION ${PROJECT_VERSION}: build=${CMAKE_BUILD_TYPE} "
               "system=${CMAKE_SYSTEM_PROCESSOR} compiler=${CMAKE_CXX_COMPILER_ID} "
               "tests=${HYPERION_BUILD_TESTS}")
