// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "harness/test_harness.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "hyperion/sigsys_diagnostics.hpp"

namespace hyperion::test {
namespace {

// Print and flush a line. The flush matters for the same reason the SIGSYS
// reporter does: a test binary that dies mid-run otherwise loses the output that
// says which test it was in.
void say_flushed(const char* text) {
    std::fputs(text, stdout);
    std::fflush(stdout);
}

// Supports `--filter=substring` and `--list` for CI ergonomics.
struct Options {
    std::string filter;
    bool list_only = false;
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        constexpr const char* kFilter = "--filter=";
        if (arg.rfind(kFilter, 0) == 0) {
            opts.filter = arg.substr(std::strlen(kFilter));
        } else if (arg == "--list") {
            opts.list_only = true;
        }
    }
    return opts;
}

bool matches(const TestCase& tc, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    return (tc.suite + '.' + tc.name).find(filter) != std::string::npos;
}

} // namespace

int run_all(const char* program_name, int argc, char** argv) {
    const Options opts = parse_options(argc, argv);
    // Report (but do not swallow) a SIGSYS: see hyperion/sigsys_diagnostics.hpp.
    // The process still dies with SIGSYS, so CTest reports the same verdict that
    // install.sh keys its offer to skip on.
    hyperion::install_sigsys_diagnostics("hyperion_unit_tests");

    if (opts.list_only) {
        for (const TestCase& tc : registry()) {
            std::printf("%s.%s\n", tc.suite.c_str(), tc.name.c_str());
        }
        return 0;
    }

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const TestCase& tc : registry()) {
        if (!matches(tc, opts.filter)) {
            ++skipped;
            continue;
        }
        const std::string label = tc.suite + '.' + tc.name;
        {
            char run_line[512];
            std::snprintf(run_line, sizeof(run_line), "[ RUN  ] %s\n", label.c_str());
            hyperion::set_sigsys_context(label.c_str());
            say_flushed(run_line);
        }
        try {
            tc.fn();
            std::printf("[ PASS ] %s\n", label.c_str());
            ++passed;
        } catch (const SkipTest& e) {
            std::printf("[ SKIP ] %s (%s)\n", label.c_str(), e.what());
            ++skipped;
        } catch (const AssertionFailure& e) {
            std::printf("[ FAIL ] %s\n         %s\n", label.c_str(), e.what());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("[ FAIL ] %s\n         unexpected exception: %s\n", label.c_str(),
                        e.what());
            ++failed;
        }
    }

    std::printf("\n%s: %d passed, %d failed, %d skipped (of %zu registered)\n", program_name,
                passed, failed, skipped, registry().size());
    return failed == 0 ? 0 : 1;
}

} // namespace hyperion::test

int main(int argc, char** argv) {
    return hyperion::test::run_all(argv[0], argc, argv);
}
