// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: execve(2). The parent forks, the child replaces its image with
// `exec_target`, and the parent verifies both the child's output path and the
// exit status the new program chose.
//
// `exec_target` is located next to this program by reading /proc/self/exe, which
// exercises the translator's redirection of that path: under a dynamic binary
// translator /proc/self/exe names the *translator*, so a translator that did not
// redirect it would hand the guest its own binary instead.
//
// Native expected output:
//   target argv1=hello
//   PASS execve status=3
// with exit status 0.
#define _GNU_SOURCE // fixtures are built with a plain `cc`; be explicit
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        printf("FAIL readlink /proc/self/exe\n");
        return 1;
    }
    self[n] = '\0';

    char* slash = strrchr(self, '/');
    if (slash == NULL) {
        printf("FAIL no directory in own path\n");
        return 1;
    }
    strcpy(slash + 1, "exec_target");

    const pid_t p = fork();
    if (p < 0) {
        printf("FAIL fork\n");
        return 1;
    }
    if (p == 0) {
        char* const argv[] = {"exec_target", "hello", NULL};
        char* const envp[] = {NULL};
        execve(self, argv, envp);
        // Reaching here means execve failed, which the test must report rather
        // than let the child quietly succeed.
        printf("FAIL execve returned\n");
        return 1;
    }

    int status = -1;
    const pid_t w = waitpid(p, &status, 0);
    const int ok = (w == p) && WIFEXITED(status) && (WEXITSTATUS(status) == 3);
    printf("%s execve status=%d\n", ok ? "PASS" : "FAIL", WEXITSTATUS(status));
    return ok ? 0 : 1;
}
