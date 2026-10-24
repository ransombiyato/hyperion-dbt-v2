# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors

add_library(hyperion_test_harness STATIC harness/test_main.cpp)
target_include_directories(hyperion_test_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hyperion_test_harness PUBLIC hyperion::core hyperion_warnings)

add_executable(hyperion_unit_tests
    unit/test_version.cpp
    unit/test_json.cpp
    unit/test_syscall_registry.cpp
    unit/test_syscall_dispatch.cpp
    unit/test_syscall_adapters.cpp
    unit/test_host_probe.cpp
    unit/test_bench_stats.cpp
    unit/test_guest_state.cpp
    unit/test_decoder.cpp
    unit/test_aarch64_encoder.cpp
    unit/test_exec_mem.cpp
    unit/test_translator.cpp
    unit/test_translation_cache.cpp
    unit/test_interpreter.cpp
    unit/test_syscall.cpp
    unit/test_runtime_helpers.cpp
    unit/test_address_space.cpp
    unit/test_elf.cpp
    unit/test_elf_loader.cpp)
target_link_libraries(hyperion_unit_tests PRIVATE hyperion_test_harness)

add_test(NAME unit.native COMMAND hyperion_unit_tests)

add_test(NAME cli.smoke
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_smoke.sh" $<TARGET_FILE:hyperion>)

add_test(NAME cli.elf
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/elf_test.sh"
            $<TARGET_FILE:hyperion>
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tiny-static.elf")

add_test(NAME cli.wrapper
    COMMAND ${CMAKE_COMMAND} -E env
            HYPERION_SRC=${CMAKE_SOURCE_DIR}
            HYPERION_BUILD_DIR=${CMAKE_BINARY_DIR}
            bash "${CMAKE_CURRENT_SOURCE_DIR}/cli/wrapper_test.sh"
            $<TARGET_FILE:hyperion>)

add_executable(hyperion_differential_tests
    differential/test_differential.cpp)
target_link_libraries(hyperion_differential_tests PRIVATE hyperion_test_harness)
add_test(NAME differential.native_vs_interpreter COMMAND hyperion_differential_tests)

# --- syscall registry CLI contract ------------------------------------------
# `hyperion syscall-registry` is the only supported way for a user to inspect or
# extend the compatibility layer, so its observable behaviour is tested directly.
add_test(NAME cli.syscall_registry
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/cli/registry_cli_test.sh"
            $<TARGET_FILE:hyperion>)
set_tests_properties(cli.syscall_registry PROPERTIES LABELS "cli;syscall" TIMEOUT 60)

# --- guest process regression tests -----------------------------------------
#
# These execute real x86-64 Linux guests, so they need a compiler that can
# produce a *static x86-64* ELF. That is the host compiler on an x86-64 machine
# and a cross compiler everywhere else. The test is only registered when such a
# compiler was found: registering it unconditionally would let a missing
# toolchain look like a passing suite, which is the opposite of what a
# regression test is for.
set(HYPERION_GUEST_CC "" CACHE STRING
    "Compiler that produces static x86-64 ELF guest fixtures (auto-detected if empty)")
set(HYPERION_BUSYBOX "" CACHE STRING
    "Static x86-64 BusyBox used by the guest shell-pipeline test (optional)")

if(NOT HYPERION_GUEST_CC)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(HYPERION_GUEST_CC "cc")
    else()
        find_program(HYPERION_GUEST_CC x86_64-linux-gnu-gcc)
    endif()
endif()

if(HYPERION_GUEST_CC)
    add_test(NAME guest.process
        COMMAND ${CMAKE_COMMAND} -E env
                HYPERION_BUSYBOX=${HYPERION_BUSYBOX}
                HYPERION_GUEST_CC=${HYPERION_GUEST_CC}
                bash "${CMAKE_CURRENT_SOURCE_DIR}/guest/process_test.sh"
                $<TARGET_FILE:hyperion>
                "${CMAKE_CURRENT_SOURCE_DIR}/guest/fixtures")
    set_tests_properties(guest.process PROPERTIES LABELS "guest" TIMEOUT 120)

    # End-to-end proof of the syscall compatibility layer: a real x86-64 guest
    # using the adapted syscalls, a user-written registry rule that enables a
    # syscall the binary does not implement, its removal, and a corrupt file that
    # must not change behaviour or be overwritten.
    add_test(NAME guest.syscall_compat
        COMMAND ${CMAKE_COMMAND} -E env
                HYPERION_GUEST_CC=${HYPERION_GUEST_CC}
                bash "${CMAKE_CURRENT_SOURCE_DIR}/guest/syscall_compat_test.sh"
                $<TARGET_FILE:hyperion>
                "${CMAKE_CURRENT_SOURCE_DIR}/guest/fixtures")
    set_tests_properties(guest.syscall_compat PROPERTIES LABELS "guest;syscall" TIMEOUT 180)
else()
    message(STATUS "HYPERION: no static x86-64 fixture compiler found; "
                   "guest.process tests not registered (set HYPERION_GUEST_CC)")
endif()

# The same suite under the Tier 1 JIT, on a host that can execute generated
# AArch64 code. Off by default: on an x86-64 host Tier 1 falls back to the
# interpreter, so running it twice would measure nothing new. Enable it with
# -DHYPERION_ENABLE_GUEST_TIER1_TESTS=ON on an AArch64 host (or under qemu).
option(HYPERION_ENABLE_GUEST_TIER1_TESTS "Also run the guest tests on Tier 1" OFF)
if(HYPERION_GUEST_CC AND HYPERION_ENABLE_GUEST_TIER1_TESTS)
    add_test(NAME guest.process.tier1
        COMMAND ${CMAKE_COMMAND} -E env
                HYPERION_BUSYBOX=${HYPERION_BUSYBOX}
                HYPERION_GUEST_CC=${HYPERION_GUEST_CC}
                HYPERION_TIER=1
                bash "${CMAKE_CURRENT_SOURCE_DIR}/guest/process_test.sh"
                $<TARGET_FILE:hyperion>
                "${CMAKE_CURRENT_SOURCE_DIR}/guest/fixtures")
    set_tests_properties(guest.process.tier1 PROPERTIES LABELS "guest" TIMEOUT 120)
endif()

option(HYPERION_ENABLE_CROSS_TESTS "Run AArch64 tests under qemu-user" OFF)
set(HYPERION_AARCH64_CXX "" CACHE STRING "AArch64 cross compiler")
set(HYPERION_QEMU_AARCH64 "" CACHE STRING "AArch64 user-mode emulator")

if(HYPERION_ENABLE_CROSS_TESTS)
    add_test(NAME cross.aarch64_tests
        COMMAND ${CMAKE_COMMAND}
            -DHYPERION_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DHYPERION_CROSS_BINARY_DIR=${CMAKE_BINARY_DIR}/cross-aarch64
            -DHYPERION_AARCH64_CXX=${HYPERION_AARCH64_CXX}
            -DHYPERION_QEMU=${HYPERION_QEMU_AARCH64}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_cross_tests.cmake)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME syscall.inventory_valid
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/gen_syscall_inventory.py --check)
endif()
