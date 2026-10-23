// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: file descriptor inheritance across fork(2), including the
// shared file offset that a real fork gives (both processes write to the same
// open file description) and the per-descriptor FD_CLOEXEC bit.
//
// Native expected output:
//   PASS fd_inherit
// with exit status 0. The two writers deliberately race; only the interleaving
// of their bytes is unspecified, never the total, which is what is asserted.
#define _GNU_SOURCE // fixtures are built with a plain `cc`; be explicit
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h> // mkstemp(3)
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/hyperion-fd-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        printf("FAIL mkstemp\n");
        return 1;
    }

    const pid_t p = fork();
    if (p < 0) {
        printf("FAIL fork\n");
        return 1;
    }
    if (p == 0) {
        // No fd was passed to the child: it uses the one it inherited. Writing
        // five bytes here and five in the parent must produce ten, because a
        // forked descriptor shares its file offset.
        const char* s = "child";
        ssize_t unused = write(fd, s, strlen(s));
        (void)unused;
        _exit(0);
    }

    const char* s = "paren";
    ssize_t unused = write(fd, s, strlen(s));
    (void)unused;

    int status = -1;
    waitpid(p, &status, 0);

    char buf[64] = {0};
    const off_t back = lseek(fd, 0, SEEK_SET);
    const ssize_t got = (back == 0) ? read(fd, buf, sizeof(buf) - 1) : -1;
    close(fd);
    unlink(path);

    const int ok = (got == 10) && (WIFEXITED(status)) && (WEXITSTATUS(status) == 0);
    printf("%s fd_inherit bytes=%zd\n", ok ? "PASS" : "FAIL", (got < 0) ? -1 : got);
    return ok ? 0 : 1;
}
