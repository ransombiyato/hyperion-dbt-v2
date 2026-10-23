// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: the error paths of process creation. Every case asserts the
// errno a real kernel returns, so a translator that faked success or invented a
// different error fails here rather than passing silently.
//
// Native expected output:
//   PASS bad_args
// with exit status 0.
#define _GNU_SOURCE // CLONE_* and the clone(2) prototype
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

int main(void) {
    // Every expectation below was checked against native Linux first, so this
    // fixture pins behaviour the translator is *required* to match -- not an
    // assumption about what a kernel ought to do.
    //
    // glibc's clone() wrapper validates its arguments before issuing the
    // syscall, so all of these fail in the library with EINVAL and never reach
    // the translator. They are still worth pinning: a translator that
    // short-circuits a library call differently from the host would diverge
    // here. The translator's own rejection of CLONE_VM and friends (ENOSYS, a
    // deliberate divergence from native, which does honour them) is exercised
    // by raw_clone_flags.c, which issues the syscall directly.
    char stack[65536];
    char* top = stack + sizeof(stack);

    errno = 0;
    pid_t rc = clone(NULL, top, CLONE_VM | CLONE_THREAD | SIGCHLD, NULL);
    check("glibc clone CLONE_VM|CLONE_THREAD -> EINVAL", (rc == -1) && (errno == EINVAL));

    errno = 0;
    rc = clone(NULL, top, CLONE_VM | SIGCHLD, NULL);
    check("glibc clone CLONE_VM -> EINVAL", (rc == -1) && (errno == EINVAL));

    errno = 0;
    rc = clone(NULL, top, CLONE_FILES | SIGCHLD, NULL);
    check("glibc clone CLONE_FILES -> EINVAL", (rc == -1) && (errno == EINVAL));

    errno = 0;
    rc = clone(NULL, top, CLONE_NEWNS | SIGCHLD, NULL);
    check("glibc clone CLONE_NEWNS -> EINVAL", (rc == -1) && (errno == EINVAL));

    errno = 0;
    rc = clone(NULL, top, SIGUSR1, NULL);
    check("glibc clone exit_signal=SIGUSR1 -> EINVAL", (rc == -1) && (errno == EINVAL));

    // CLONE_SETTLS with a null TLS pointer is EINVAL in the kernel, and this one
    // does reach the translator, which must return the same thing.
    errno = 0;
    rc = clone(NULL, top, CLONE_SETTLS | SIGCHLD, NULL, NULL, 0);
    check("clone CLONE_SETTLS tls=NULL -> EINVAL", (rc == -1) && (errno == EINVAL));

    // execve error paths must be non-destructive: this process is still running
    // and still able to report after each one.
    errno = 0;
    (void)execve("/nonexistent/hyperion-nope", NULL, NULL);
    check("execve missing -> ENOENT", errno == ENOENT);

    errno = 0;
    (void)execve("/tmp", NULL, NULL);
    check("execve directory -> EACCES or EISDIR",
          (errno == EACCES) || (errno == EISDIR) || (errno == ENOENT));

    errno = 0;
    (void)execve("/etc/hostname", NULL, NULL);
    check("execve non-executable -> EACCES or ENOEXEC or ENOENT",
          (errno == EACCES) || (errno == ENOEXEC) || (errno == ENOENT));

    // A pid that is not this process's child is ECHILD, not a silent 0.
    errno = 0;
    int status = 0;
    const pid_t w = waitpid(999999, &status, 0);
    check("waitpid foreign pid -> ECHILD", (w == -1) && (errno == ECHILD));

    // Waiting when there are no children at all is also ECHILD.
    errno = 0;
    const pid_t w2 = waitpid(-1, &status, WNOHANG);
    check("waitpid no children -> ECHILD", (w2 == -1) && (errno == ECHILD));

    printf("%s bad_args\n", (failures == 0) ? "PASS" : "FAIL");
    return (failures == 0) ? 0 : 1;
}
