// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: vfork(2). The child is restricted to what the vfork contract
// actually permits -- _exit() or execve() -- because a real vfork shares the
// parent's address space and any other write would be undefined behaviour. The
// parent is expected to be suspended until the child exits, then to reap it.
//
// Native expected output:
//   PASS vfork status=9
// with exit status 0.
#define _GNU_SOURCE // fixtures are built with a plain `cc`; be explicit
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    const pid_t p = vfork();
    if (p < 0) {
        printf("FAIL vfork\n");
        return 1;
    }
    if (p == 0) {
        _exit(9); // the only safe thing a vfork child can do besides execve
    }

    int status = -1;
    const pid_t w = waitpid(p, &status, 0);
    const int ok = (w == p) && WIFEXITED(status) && (WEXITSTATUS(status) == 9);
    printf("%s vfork status=%d\n", ok ? "PASS" : "FAIL", WEXITSTATUS(status));
    return ok ? 0 : 1;
}
