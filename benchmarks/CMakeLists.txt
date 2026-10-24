# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Benchmark harness (Phase 1 scaffolding).
#
# IMPORTANT: HYPERION has no guest execution engine yet. This harness measures
# only *host* and *harness* characteristics and records them in a machine
# readable report. It deliberately reports "unavailable" rather than inventing
# guest-execution numbers. See BENCHMARKS.md for the full methodology.

add_executable(hyperion_bench host_bench.cpp)
target_link_libraries(hyperion_bench PRIVATE hyperion::core hyperion_warnings)

# Tier 0 vs Tier 1 benchmark (Phase 5). Tier 1 numbers appear only on a host that
# can execute generated AArch64 code; elsewhere they are reported unavailable.
add_executable(hyperion_tier_bench tier_bench.cpp)
target_link_libraries(hyperion_tier_bench PRIVATE hyperion::core hyperion_warnings)

# Syscall dispatch microbenchmark (Phase 6 compatibility layer). Measures the cost
# of one dispatch through each layer against a bare host syscall.
add_executable(hyperion_syscall_bench syscall_bench.cpp)
target_link_libraries(hyperion_syscall_bench PRIVATE hyperion::core hyperion_warnings)

add_test(NAME syscall_bench.smoke
    COMMAND hyperion_syscall_bench --iterations 20000 --repeats 3 --json)
set_tests_properties(syscall_bench.smoke PROPERTIES LABELS "bench;smoke;syscall")

# The benchmark must reject a single sample rather than publish a spread it
# cannot compute - same rule as the host harness.
add_test(NAME syscall_bench.rejects_single_sample
    COMMAND ${CMAKE_COMMAND}
        -DHYPERION_BENCH_BIN=$<TARGET_FILE:hyperion_syscall_bench>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../tests/bench_rejects_bad_args.cmake")
set_tests_properties(syscall_bench.rejects_single_sample PROPERTIES LABELS "bench;syscall")

# Correctness smoke: both tiers produce the right result for a small loop. Runs
# on every host (on x86-64 only tier 0 executes; the script tolerates that).
add_test(NAME tier_bench.smoke COMMAND hyperion_tier_bench --iterations 1000 --repeats 3 --json)
set_tests_properties(tier_bench.smoke PROPERTIES LABELS "bench;smoke")

add_test(NAME bench.smoke COMMAND hyperion_bench --iterations 1000 --repeats 3 --json)
set_tests_properties(bench.smoke PROPERTIES LABELS "bench;smoke")

# Exercises the reporting path end to end: repeated samples, warm-up discard and
# a stable checksum. Fails (non-zero) if the checksum is unstable, which would
# mean the harness measured a broken build.
add_test(NAME bench.methodology COMMAND hyperion_bench --iterations 20000 --repeats 5 --json)
set_tests_properties(bench.methodology PROPERTIES LABELS "bench")

# Regression guard for BENCH-001: the optimiser must not be able to eliminate
# the timed workload. A larger iteration count is used so that an eliminated
# workload is unambiguously implausible rather than merely fast, and the harness
# exits non-zero when the timing is implausible.
add_test(NAME bench.timing_not_eliminated
    COMMAND hyperion_bench --iterations 20000000 --repeats 3 --json)
set_tests_properties(bench.timing_not_eliminated PROPERTIES LABELS "bench")

# A single sample cannot express a spread; the harness must reject it rather
# than silently report a meaningless one. Driven by a script because CTest's
# WILL_FAIL and PASS_REGULAR_EXPRESSION invert each other.
add_test(NAME bench.rejects_single_sample
    COMMAND ${CMAKE_COMMAND}
        -DHYPERION_BENCH_BIN=$<TARGET_FILE:hyperion_bench>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../tests/bench_rejects_bad_args.cmake")
set_tests_properties(bench.rejects_single_sample PROPERTIES LABELS "bench")
