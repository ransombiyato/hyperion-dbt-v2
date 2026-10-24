// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/host_syscalls.hpp"

#include <cstdlib>
#include <cstring>

#if defined(__linux__)
// SYS_statx / SYS_memfd_create existence is what decides whether the native
// paths are even expressible; the numbers are only defined by this header.
#include <sys/syscall.h>
#endif

#if defined(__ANDROID__)
// Declared by bionic (libc.so) since API 1. Declared here rather than by
// including <sys/system_properties.h> so that this file also builds against a
// libc without the Android property API.
extern "C" int __system_property_get(const char* name, char* value);
#endif

namespace hyperion {
namespace {

#if defined(__ANDROID__)
// Android 11 is the first release whose bionic syscall list - and therefore the
// zygote seccomp allowlist built from it - contains statx and memfd_create.
constexpr int kAndroidApiStatxAndMemfd = 30;
#endif

bool env_flag_set(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return false;
    }
    return std::strcmp(value, "0") != 0;
}

} // namespace

int host_android_api_level() {
#if defined(__ANDROID__)
    static const int level = [] {
        // PROP_VALUE_MAX is 92 in bionic; a longer buffer is harmless.
        char value[128] = {};
        if (__system_property_get("ro.build.version.sdk", value) <= 0) {
            return 0;
        }
        const int parsed = std::atoi(value);
        return parsed > 0 ? parsed : 0;
    }();
    return level;
#else
    return 0;
#endif
}

bool host_portable_syscalls_forced() {
    static const bool forced = env_flag_set("HYPERION_PORTABLE_SYSCALLS");
    return forced;
}

bool host_statx_syscall_usable() {
#if defined(SYS_statx) || defined(__NR_statx)
    if (host_portable_syscalls_forced()) {
        return false;
    }
#if defined(__ANDROID__)
    // The zygote filter is built from the *device's* bionic syscall list, so the
    // decision has to be made at run time from the device's API level - the API
    // level this binary was compiled against says nothing about the device.
    const int api = host_android_api_level();
    if (api == 0) {
        // Unknown API level on Android: assume the syscall is not allowed. A
        // wrong "yes" is a fatal SIGSYS; a wrong "no" costs a statx that is
        // synthesized from fstatat() instead.
        return false;
    }
    return api >= kAndroidApiStatxAndMemfd;
#else
    return true;
#endif
#else
    // No syscall number for statx in this libc's headers.
    return false;
#endif
}

bool host_memfd_create_syscall_usable() {
#if defined(SYS_memfd_create) || defined(__NR_memfd_create)
    if (host_portable_syscalls_forced()) {
        return false;
    }
#if defined(__ANDROID__)
    const int api = host_android_api_level();
    if (api == 0) {
        return false;
    }
    return api >= kAndroidApiStatxAndMemfd;
#else
    return true;
#endif
#else
    return false;
#endif
}

} // namespace hyperion
