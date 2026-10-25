// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Temporary directories and global-state guards for the test suite.
//
// Why this exists
// ---------------
// The tests used to build temporary paths from a hardcoded "/tmp". That is a
// Linux-desktop assumption, not a portable one:
//
//   - Termux on Android has no /tmp at all. Its writable temporary directory is
//     $TMPDIR (/data/data/com.termux/files/usr/tmp), and /tmp cannot be created
//     without root.
//   - Other constrained hosts (containers, read-only roots, some Android
//     app-private sandboxes) also lack a usable /tmp.
//
// On such a host every mkstemp("/tmp/...")/mkdtemp("/tmp/...") returns ENOENT,
// which turned into "fixture creation returned nullptr" and made whole groups of
// tests fail for a reason that had nothing to do with the code under test.
//
// Every temporary path in the suite therefore comes from TempDir, which picks
// the first usable base directory ($TMPDIR, /tmp, $HOME/.cache,
// $PREFIX/tmp, then the current directory) and creates a private directory
// inside it that is removed - recursively - when the enclosing test ends.
//
// Global state
// ------------
// Two syscalls the suite tests mutate process-wide state: chdir() and umask().
// A test that fails its assertion between "change it" and "change it back"
// leaves the *rest of the binary* running in the wrong directory or with the
// wrong mask, which shows up as unrelated failures further down the run.
// ScopedCwd and ScopedUmask restore the previous state from their destructors,
// so the restore also happens when an assertion throws or a test is skipped.
#ifndef HYPERION_TEST_TEMPDIR_HPP
#define HYPERION_TEST_TEMPDIR_HPP

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "harness/test_harness.hpp"

namespace hyperion::test {
namespace detail {

inline bool is_directory(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A base directory usable for temporary files, or "" when none is found.
inline std::string pick_temp_base() {
    std::vector<std::string> candidates;
    if (const char* tmpdir = std::getenv("TMPDIR"); tmpdir != nullptr && *tmpdir != '\0') {
        candidates.emplace_back(tmpdir);
    }
    candidates.emplace_back("/tmp");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        candidates.emplace_back(std::string(home) + "/.cache");
    }
    // Termux sets PREFIX to its package root; its tmp/ is writable by the user.
    if (const char* prefix = std::getenv("PREFIX"); prefix != nullptr && *prefix != '\0') {
        candidates.emplace_back(std::string(prefix) + "/tmp");
    }
    candidates.emplace_back(".");

    for (const std::string& candidate : candidates) {
        if (!is_directory(candidate)) {
            continue;
        }
        // Being a directory is not enough: it has to be writable by this
        // process. access(2) is the portable way to find out without leaving
        // anything behind.
        if (::access(candidate.c_str(), W_OK | X_OK) == 0) {
            return candidate;
        }
    }
    return {};
}

// Recursive removal, without <filesystem>: a few files per test is not worth a
// link-time dependency, and this works identically under bionic and glibc.
inline void remove_tree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        (void)::unlink(path.c_str());
        return;
    }
    std::vector<std::string> entries;
    while (const dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }
        entries.emplace_back(name);
    }
    (void)::closedir(dir);
    for (const std::string& name : entries) {
        const std::string child = path + "/" + name;
        struct stat st{};
        if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(child);
        } else {
            (void)::unlink(child.c_str());
        }
    }
    (void)::rmdir(path.c_str());
}

} // namespace detail

// A mkstemp()/mkdtemp() template that lives in the usable temporary directory
// rather than in a hardcoded /tmp. The returned string is owned by the caller
// and can be passed to mkstemp after .data() has made it mutable.
inline std::string temp_template(const char* tag) {
    return detail::pick_temp_base() + "/hyperion-" + tag + "-XXXXXX";
}

// A private, empty directory that is deleted (recursively) when the test ends.
class TempDir {
public:
    explicit TempDir(const char* prefix = "hyperion-test") {
        const std::string base = detail::pick_temp_base();
        if (base.empty()) {
            HYPERION_CHECK(false && "no writable temporary directory: set TMPDIR");
        }
        std::string pattern = base + "/" + prefix + "-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* made = ::mkdtemp(buffer.data());
        if (made == nullptr) {
            HYPERION_CHECK(false && "mkdtemp failed; TMPDIR is not writable");
        }
        path_ = made;
    }

    ~TempDir() {
        if (!path_.empty()) {
            detail::remove_tree(path_);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    const std::string& path() const { return path_; }
    std::string file(const std::string& name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

// Restores the process working directory on destruction, even when an assertion
// throws (which is the normal way a test aborts).
class ScopedCwd {
public:
    ScopedCwd() {
        if (::getcwd(saved_, sizeof(saved_)) == nullptr) {
            // An unreadable cwd is not something a test should silently work
            // around: report it.
            HYPERION_CHECK(false && "getcwd failed; cannot restore the working directory");
        }
        have_saved_ = true;
    }
    ~ScopedCwd() {
        // glibc declares chdir() with warn_unused_result, which a (void) cast
        // does not silence - and rightly so: failing to get back to the original
        // directory would leave the rest of the binary in the wrong place, which
        // is the bug this class exists to prevent. A destructor cannot throw, so
        // report instead of pretending.
        if (have_saved_ && ::chdir(saved_) != 0) {
            std::fputs("hyperion tests: could not restore the working directory\n", stderr);
        }
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

    const char* saved() const { return saved_; }

private:
    char saved_[4096] = {};
    bool have_saved_ = false;
};

// Restores the process file-mode creation mask on destruction. Never leave a
// test's mask in place: it changes the permissions of every file the *rest* of
// the binary creates.
class ScopedUmask {
public:
    explicit ScopedUmask(mode_t mask) : saved_(::umask(mask)) {}
    ~ScopedUmask() { (void)::umask(saved_); }
    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

    mode_t saved() const { return saved_; }

private:
    mode_t saved_;
};

} // namespace hyperion::test

#endif // HYPERION_TEST_TEMPDIR_HPP
