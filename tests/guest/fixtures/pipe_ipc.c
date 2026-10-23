// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: pipe(2) plus fork(2), the mechanism every shell pipeline is
// built from. The child writes to the pipe, the parent reads it back.
//
// Native expected output:
//   PASS pipe got=5
// with exit status 0.
#define _GNU_SOURCE // fixtures are built with a plain `cc`; be explicit
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        printf("FAIL pipe\n");
        return 1;
    }

    const pid_t p = fork();
    if (p < 0) {
        printf("FAIL fork\n");
        return 1;
    }
    if (p == 0) {
        close(fds[0]); // the child only writes
        const char* msg = "hello";
        ssize_t unused = write(fds[1], msg, strlen(msg));
        (void)unused;
        close(fds[1]); // closing the last write end is what ends the parent's read
        _exit(0);
    }

    close(fds[1]); // the parent only reads
    char buf[64] = {0};
    const ssize_t got = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);

    int status = -1;
    waitpid(p, &status, 0);

    const int ok = (got == 5) && (memcmp(buf, "hello", 5) == 0) && WIFEXITED(status) &&
                   (WEXITSTATUS(status) == 0);
    printf("%s pipe got=%zd\n", ok ? "PASS" : "FAIL", (got < 0) ? -1 : got);
    return ok ? 0 : 1;
}
