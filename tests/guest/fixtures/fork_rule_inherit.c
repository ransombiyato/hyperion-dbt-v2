// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: call a registry-provided syscall number from a *child* process.
//
// 300 is unassigned on x86-64; the test's registry maps it onto getpid(2). The
// parent calls it, forks, and the child calls it again. Before the child
// inherited the parent's compiled rules, the parent's call succeeded and the
// child's silently became -ENOSYS - a failure that looks like an unrelated bug
// in the child rather than a lost configuration.
//
// Output is one line so the shell test can compare it exactly.
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int call_300(void) {
    errno = 0;
    const long result = syscall(300);
    if (result > 0) {
        return 1; // mapped onto getpid(2), which is always positive
    }
    return 0;
}

int main(void) {
    const int parent_ok = call_300();
    const pid_t pid = fork();
    if (pid < 0) {
        printf("fork=FAILED\n");
        return 0;
    }
    if (pid == 0) {
        /* _exit, not exit: the child shares the parent's stdio buffers otherwise,
         * and this fixture must print exactly one line per process. */
        _exit(call_300() ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        printf("waitpid=FAILED\n");
        return 0;
    }
    const int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("parent=%d child=%d\n", parent_ok, child_ok);
    return 0;
}
