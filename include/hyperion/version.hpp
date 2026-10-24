// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Build and version identification for the HYPERION translator.
#ifndef HYPERION_VERSION_HPP
#define HYPERION_VERSION_HPP

#include <string>

namespace hyperion {

// The version comes from CMakeLists.txt (project(... VERSION ...)) through the
// HYPERION_VERSION_* compile definitions below, so the CLI, `hyperion version`
// and the packaging metadata cannot drift apart. The fallback values only apply
// to a translation unit compiled outside CMake.
#ifndef HYPERION_VERSION_MAJOR
#define HYPERION_VERSION_MAJOR 0
#endif
#ifndef HYPERION_VERSION_MINOR
#define HYPERION_VERSION_MINOR 2
#endif
#ifndef HYPERION_VERSION_PATCH
#define HYPERION_VERSION_PATCH 0
#endif

inline constexpr int kVersionMajor = HYPERION_VERSION_MAJOR;
inline constexpr int kVersionMinor = HYPERION_VERSION_MINOR;
inline constexpr int kVersionPatch = HYPERION_VERSION_PATCH;

// Semantic version string, e.g. "0.2.0".
std::string version_string();

// Human-readable one-line build identity including compiler and build type.
// Recorded in benchmark output so measurements are attributable to a build.
std::string build_summary();

} // namespace hyperion

#endif // HYPERION_VERSION_HPP
