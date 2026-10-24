# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# CMake driver script: configure + build + run the test suite for AArch64
# under qemu-user. Invoked as a CTest test by the native build tree.
#
# Required variables:
#   HYPERION_SOURCE_DIR       - repository root
#   HYPERION_CROSS_BINARY_DIR - scratch build tree for the cross build
#   HYPERION_AARCH64_CXX      - AArch64 C++ compiler (e.g. aarch64-linux-gnu-g++)
#   HYPERION_QEMU             - AArch64 user-mode emulator (e.g. qemu-aarch64)

foreach(required HYPERION_SOURCE_DIR HYPERION_CROSS_BINARY_DIR HYPERION_AARCH64_CXX HYPERION_QEMU)
    if(NOT ${required})
        message(FATAL_ERROR "cross tests: '${required}' is not set")
    endif()
endforeach()

find_program(HYPERION_NINJA ninja)
if(NOT HYPERION_NINJA)
    message(FATAL_ERROR "cross tests: 'ninja' not found")
endif()

set(toolchain "${HYPERION_SOURCE_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake")

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S "${HYPERION_SOURCE_DIR}"
        -B "${HYPERION_CROSS_BINARY_DIR}"
        -G Ninja
        -DCMAKE_TOOLCHAIN_FILE=${toolchain}
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
        -DCMAKE_CXX_COMPILER=${HYPERION_AARCH64_CXX}
        -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
        -DHYPERION_BUILD_BENCHMARKS=OFF
    RESULT_VARIABLE cfg_result
    OUTPUT_VARIABLE cfg_output
    ERROR_VARIABLE cfg_error)
if(NOT cfg_result EQUAL 0)
    message(FATAL_ERROR "AArch64 configure failed:\n${cfg_output}\n${cfg_error}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${HYPERION_CROSS_BINARY_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "AArch64 build failed:\n${build_output}\n${build_error}")
endif()

set(test_binary "${HYPERION_CROSS_BINARY_DIR}/tests/hyperion_unit_tests")
if(NOT EXISTS "${test_binary}")
    message(FATAL_ERROR "cross tests: expected test binary not found at ${test_binary}")
endif()

execute_process(
    COMMAND ${HYPERION_QEMU} "${test_binary}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
message(STATUS "AArch64 test output:\n${run_output}")
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "AArch64 tests failed (exit ${run_result}):\n${run_error}")
endif()

# Also run the hardware-differential binary. On AArch64 it cannot compare
# against real x86-64, so every case must report SKIP (exit 0, zero passes).
# This is a deliberate integrity check: it proves the suite does not claim
# coverage it did not achieve on a non-x86-64 host.
set(diff_binary "${HYPERION_CROSS_BINARY_DIR}/tests/hyperion_differential_tests")
if(NOT EXISTS "${diff_binary}")
    message(FATAL_ERROR "cross tests: differential binary not found at ${diff_binary}")
endif()

execute_process(
    COMMAND ${HYPERION_QEMU} "${diff_binary}"
    RESULT_VARIABLE diff_result
    OUTPUT_VARIABLE diff_output
    ERROR_VARIABLE diff_error)
message(STATUS "AArch64 differential output:\n${diff_output}")
if(NOT diff_result EQUAL 0)
    message(FATAL_ERROR "AArch64 differential run failed (exit ${diff_result}):\n${diff_error}")
endif()
if(NOT diff_output MATCHES "0 passed")
    message(FATAL_ERROR
        "AArch64 differential suite reported a pass without an x86-64 oracle; "
        "it must SKIP every case. Output:\n${diff_output}")
endif()

# Also run the CLI smoke test against the cross-built binary, under qemu. This
# is the end-to-end check that tier 1 generates AArch64 code and executes it:
# the script requires tier 1 to have translated a non-zero number of blocks and
# to agree with the interpreter on the step count.
set(cli_script "${HYPERION_SOURCE_DIR}/tests/cli/cli_smoke.sh")
set(cli_binary "${HYPERION_CROSS_BINARY_DIR}/hyperion")
if(NOT EXISTS "${cli_binary}")
    message(FATAL_ERROR "cross tests: CLI binary not found at ${cli_binary}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
        "HYPERION_EMULATOR=${HYPERION_QEMU}"
        bash "${cli_script}" "${cli_binary}"
    RESULT_VARIABLE cli_result
    OUTPUT_VARIABLE cli_output
    ERROR_VARIABLE cli_error)
message(STATUS "AArch64 CLI smoke output:\n${cli_output}${cli_error}")
if(NOT cli_result EQUAL 0)
    message(FATAL_ERROR "AArch64 CLI smoke failed (exit ${cli_result})")
endif()

# Process tests use real static x86-64 guest fixtures. Run the AArch64 Hyperion
# binary through qemu at both execution tiers; Tier 1 must execute the generated
# AArch64 code, not silently fall back to the interpreter.
execute_process(COMMAND uname -m OUTPUT_VARIABLE host_machine OUTPUT_STRIP_TRAILING_WHITESPACE)
if(host_machine STREQUAL "x86_64")
    set(guest_cc "cc")
else()
    find_program(guest_cc NAMES x86_64-linux-gnu-gcc)
    if(NOT guest_cc)
        message(FATAL_ERROR "cross tests: x86_64-linux-gnu-gcc is required to build x86-64 guests")
    endif()
endif()
set(process_script "${HYPERION_SOURCE_DIR}/tests/guest/process_test.sh")
set(process_fixtures "${HYPERION_SOURCE_DIR}/tests/guest/fixtures")
set(busybox_path "$ENV{HYPERION_BUSYBOX}")
foreach(tier IN ITEMS 0 1)
    set(process_test_env
        "HYPERION_GUEST_CC=${guest_cc}"
        "HYPERION_TIER=${tier}"
        "HYPERION_EMULATOR=${HYPERION_QEMU}")
    if(NOT busybox_path STREQUAL "" AND EXISTS "${busybox_path}")
        list(APPEND process_test_env "HYPERION_BUSYBOX=${busybox_path}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env ${process_test_env}
            bash "${process_script}" "${cli_binary}" "${process_fixtures}" "${HYPERION_QEMU}"
        RESULT_VARIABLE process_result
        OUTPUT_VARIABLE process_output
        ERROR_VARIABLE process_error)
    message(STATUS "AArch64 guest process tests (tier ${tier}):\n${process_output}${process_error}")
    if(NOT process_result EQUAL 0)
        message(FATAL_ERROR "AArch64 guest process tests failed at tier ${tier} (exit ${process_result})")
    endif()
endforeach()

# The syscall compatibility layer gets the same treatment as process creation:
# the AArch64-built hyperion runs the guest-level compat suite through qemu at
# both tiers. Tier 1 matters here specifically, because the adapters translate
# structures the JIT never sees - a mistake would show up as wrong *guest*
# behaviour only when the guest executes generated code.
set(compat_script "${HYPERION_SOURCE_DIR}/tests/guest/syscall_compat_test.sh")
foreach(tier IN ITEMS 0 1)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
            "HYPERION_GUEST_CC=${guest_cc}"
            "HYPERION_TIER=${tier}"
            "HYPERION_EMULATOR=${HYPERION_QEMU}"
            bash "${compat_script}" "${cli_binary}" "${process_fixtures}" "${HYPERION_QEMU}"
        RESULT_VARIABLE compat_result
        OUTPUT_VARIABLE compat_output
        ERROR_VARIABLE compat_error)
    message(STATUS "AArch64 syscall compat tests (tier ${tier}):\n${compat_output}${compat_error}")
    if(NOT compat_result EQUAL 0)
        message(FATAL_ERROR
            "AArch64 syscall compat tests failed at tier ${tier} (exit ${compat_result})")
    endif()
endforeach()

# At tier 1 the point is that the JIT really executed the guest, not that it fell
# back to the interpreter and passed anyway. cli_smoke.sh already asserts this for
# a hand-written blob; do it once more for a real program that leans on the
# compat layer, because "status: ok" alone would be satisfied by the fallback.
set(compat_fixture "${HYPERION_CROSS_BINARY_DIR}/compat_probe")
execute_process(
    COMMAND ${guest_cc} -O2 -static -o "${compat_fixture}"
        "${HYPERION_SOURCE_DIR}/tests/guest/fixtures/compat_syscalls.c"
    RESULT_VARIABLE cc_result
    ERROR_VARIABLE cc_error)
if(NOT cc_result EQUAL 0)
    message(FATAL_ERROR "cross tests: cannot compile compat fixture:\n${cc_error}")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HYPERION_SYSCALL_REGISTRY=${HYPERION_CROSS_BINARY_DIR}/tier1-registry.json"
        "${HYPERION_QEMU}" "${cli_binary}" run --tier=1 "${compat_fixture}"
    RESULT_VARIABLE tier1_result
    OUTPUT_VARIABLE tier1_output
    ERROR_VARIABLE tier1_error)
message(STATUS "AArch64 tier 1 compat run:\n${tier1_output}${tier1_error}")
if(NOT tier1_result EQUAL 0)
    message(FATAL_ERROR "AArch64 tier 1 compat run failed (exit ${tier1_result})")
endif()
if(NOT tier1_output MATCHES "failures 0")
    message(FATAL_ERROR "AArch64 tier 1 compat run reported guest failures:\n${tier1_output}")
endif()
# The "tier 1: N basic block(s) translated" line is printed on stdout, while the
# fallback warning goes to stderr; search both, so the check cannot pass or fail
# on which stream a message happens to use.
set(tier1_all "${tier1_output}${tier1_error}")
if(NOT tier1_all MATCHES "tier 1: [1-9][0-9]* basic block")
    message(FATAL_ERROR
        "AArch64 tier 1 compat run did not execute generated code - it fell back to "
        "the interpreter, so this run proves nothing about the JIT. Output:\n"
        "${tier1_all}")
endif()
if(tier1_all MATCHES "falling back")
    message(FATAL_ERROR
        "AArch64 tier 1 compat run reported a fallback to the interpreter:\n${tier1_all}")
endif()

# The registry CLI is the user-facing half of the layer; run it under qemu too, so
# a path-resolution or permission difference on the AArch64 build cannot hide.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HYPERION_EMULATOR=${HYPERION_QEMU}"
        bash "${HYPERION_SOURCE_DIR}/tests/cli/registry_cli_test.sh" "${cli_binary}"
    RESULT_VARIABLE registry_result
    OUTPUT_VARIABLE registry_output
    ERROR_VARIABLE registry_error)
message(STATUS "AArch64 registry CLI tests:\n${registry_output}${registry_error}")
if(NOT registry_result EQUAL 0)
    message(FATAL_ERROR "AArch64 registry CLI tests failed (exit ${registry_result})")
endif()

message(STATUS "cross-architecture validation pipeline OK")
