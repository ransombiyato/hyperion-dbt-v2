// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: the raw clone(2) syscall, bypassing glibc's argument
// validation, to reach the translator's own clone handling.
//
// THIS FIXTURE DELIBERATELY DOES NOT MATCH NATIVE LINUX. On a real kernel,
// clone(CLONE_VM | SIGCHLD) with a null stack succeeds and creates a process
// that shares the caller's address space. HYPERION refuses it with ENOSYS
// instead, because it cannot provide a shared guest address space and a silent
// fork would hand the guest a private copy it believes is shared -- corrupting
// the guest rather than failing cleanly. The divergence is documented in
// COMPATIBILITY.md, and this test pins it so it cannot regress into a fake
// success unnoticed.
//
// Expected output under HYPERION:
//   PASS raw_clone_flags
// with exit status 0. Under native Linux this fixture FAILS by design.
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures = 0;

static void expect_enosys(const char* name, unsigned long flags) {
    errno = 0;
    const long rc = syscall(SYS_clone, flags, 0UL, 0UL, 0UL, 0UL);
    if (rc == 0) {
        _exit(0); // should not happen; if it does, do not print in the child
    }
    const int ok = (rc == -1) && (errno == ENOSYS);
    printf("%s %s (rc=%ld errno=%d)\n", ok ? "ok  " : "FAIL", name, rc, (rc < 0) ? errno : 0);
    if (!ok) {
        failures++;
    }
}

int main(void) {
    expect_enosys("raw clone CLONE_VM", CLONE_VM | SIGCHLD);
    expect_enosys("raw clone CLONE_THREAD", CLONE_THREAD | CLONE_SIGHAND | SIGCHLD);
    expect_enosys("raw clone CLONE_FILES", CLONE_FILES | SIGCHLD);
    expect_enosys("raw clone CLONE_FS", CLONE_FS | SIGCHLD);
    expect_enosys("raw clone CLONE_SIGHAND", CLONE_SIGHAND | SIGCHLD);
    // Namespace creation needs CAP_SYS_ADMIN, which unrooted Android/Termux does
    // not have, so these are refused rather than attempted.
    expect_enosys("raw clone CLONE_NEWNS", CLONE_NEWNS | SIGCHLD);
    expect_enosys("raw clone CLONE_NEWPID", CLONE_NEWPID | SIGCHLD);
    // An exit signal other than SIGCHLD needs stop/continue handling that is not
    // implemented.
    expect_enosys("raw clone exit_signal=SIGUSR1", SIGUSR1);

    printf("%s raw_clone_flags\n", (failures == 0) ? "PASS" : "FAIL");
    return (failures == 0) ? 0 : 1;
}
