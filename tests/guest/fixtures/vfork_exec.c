// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// A vfork child that execs must release its parent at exec time, not only when
// the new guest image eventually exits. The re-exec through /proc/self/exe also
// checks that a forked handler retained the guest image path.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        char byte = 0;
        if (read(3, &byte, 1) != 1) {
            return 11;
        }
        printf("child received %c\n", byte);
        return 0;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return 2;
    }

    const pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork");
        return 3;
    }
    if (pid == 0) {
        close(pipefd[1]);
        char *child_argv[] = {argv[0], (char *)"child", NULL};
        execve("/proc/self/exe", child_argv, environ);
        _exit(100 + errno);
    }

    // This write can happen only once the child has successfully execed. The
    // re-execed child waits on this byte, so a vfork implementation that waits
    // for child exit instead of exec will deterministically deadlock.
    close(pipefd[0]);
    if (write(pipefd[1], "X", 1) != 1) {
        return 4;
    }
    close(pipefd[1]);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 5;
    }
    puts("PASS vfork exec resumed");
    return 0;
}
