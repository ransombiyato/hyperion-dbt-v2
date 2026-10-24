// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "harness/test_harness.hpp"
#include "hyperion/version.hpp"

#include <algorithm>
#include <cctype>
#include <string>

using hyperion::build_summary;
using hyperion::version_string;

HYPERION_TEST("version", "version_string_is_semver") {
    const std::string v = version_string();
    HYPERION_CHECK(!v.empty());
    // MAJOR.MINOR.PATCH: exactly two dots, digits only, no whitespace.
    HYPERION_CHECK_EQ(static_cast<int>(std::count(v.begin(), v.end(), '.')), 2);
    HYPERION_CHECK(v.find(' ') == std::string::npos);
    HYPERION_CHECK(std::all_of(v.begin(), v.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.';
    }));
}

HYPERION_TEST("version", "version_string_matches_the_cmake_project_version") {
    // The header used to hardcode 0.1.0 while CMake said 0.2.0. The numbers now
    // come from the CMake project() call through compile definitions.
    const std::string expected = std::to_string(hyperion::kVersionMajor) + "." +
                                 std::to_string(hyperion::kVersionMinor) + "." +
                                 std::to_string(hyperion::kVersionPatch);
    HYPERION_CHECK_EQ(version_string(), expected);
#ifdef HYPERION_VERSION_MAJOR
    HYPERION_CHECK_EQ(hyperion::kVersionMajor, HYPERION_VERSION_MAJOR);
    HYPERION_CHECK_EQ(hyperion::kVersionMinor, HYPERION_VERSION_MINOR);
    HYPERION_CHECK_EQ(hyperion::kVersionPatch, HYPERION_VERSION_PATCH);
#endif
}

HYPERION_TEST("version", "build_summary_mentions_version") {
    const std::string s = build_summary();
    HYPERION_CHECK(s.find(version_string()) != std::string::npos);
    HYPERION_CHECK(s.find("hyperion") != std::string::npos);
}

HYPERION_TEST("version", "build_summary_names_a_compiler") {
    const std::string s = build_summary();
    const bool known = s.find("gcc") != std::string::npos || s.find("clang") != std::string::npos ||
                       s.find("unknown-compiler") != std::string::npos;
    HYPERION_CHECK(known);
}
