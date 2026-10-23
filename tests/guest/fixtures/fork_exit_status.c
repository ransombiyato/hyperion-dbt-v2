// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: fork(2) plus waitpid(2), checking that the child's exit status
// reaches the parent intact.
//
// Deterministic by construction: the child returns a status no other path can
// produce, and the parent only prints PASS when the status it observed matches.
// Native expected output is exactly:
//   child running
//   PASS fork status=7 pid_match=1
// with exit status 0.
#define _GNU_SOURCE // fixtures are built with a plain `cc`; be explicit
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    const pid_t self = getpid();
    const pid_t p = fork();
    if (p < 0) {
        printf("FAIL fork errno\n");
        return 1;
    }
    if (p == 0) {
        // The child must observe a fork() return of 0. A translator that left the
        // syscall number in the return register would make the child take the
        // parent's branch and wait on a pid that is not its child.
        printf("child running\n");
        return 7;
    }

    int status = -1;
    const pid_t w = waitpid(p, &status, 0);
    const int ok = (w == p) && WIFEXITED(status) && (WEXITSTATUS(status) == 7);
    printf("%s fork status=%d pid_match=%d\n", ok ? "PASS" : "FAIL", WEXITSTATUS(status),
           (w == p) ? 1 : 0);
    (void)self;
    return ok ? 0 : 1;
}
