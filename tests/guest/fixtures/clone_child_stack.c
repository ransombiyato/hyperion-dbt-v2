// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// A process-creating clone must honor its child_stack argument. The return
// address for the libc syscall wrapper is planted at the top of this stack, so
// a correctly emulated child enters clone_child_entry on the supplied stack.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char child_stack[64 * 1024] __attribute__((aligned(16)));

__attribute__((noinline, noreturn)) static void clone_child_entry(void) {
    volatile unsigned char marker = 0;
    const uintptr_t local = (uintptr_t)&marker;
    const uintptr_t low = (uintptr_t)child_stack;
    const uintptr_t high = low + sizeof(child_stack);
    _exit((local >= low && local < high) ? 23 : 99);
}

int main(void) {
    const uintptr_t aligned_top = ((uintptr_t)(child_stack + sizeof(child_stack))) & ~(uintptr_t)0xF;
    uintptr_t *child_sp = (uintptr_t *)(aligned_top - 16);
    child_sp[0] = (uintptr_t)clone_child_entry;
    child_sp[1] = 0;

    const long pid = syscall(SYS_clone, (unsigned long)SIGCHLD, child_sp, 0, 0, 0);
    if (pid == 0) {
        // Reached only if the emulator ignored child_stack and returned through
        // the parent's original stack. The helper detects that and exits 99.
        clone_child_entry();
    }
    if (pid < 0) {
        perror("clone");
        return 2;
    }

    int status = 0;
    if (waitpid((pid_t)pid, &status, 0) != (pid_t)pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 23) {
        fprintf(stderr, "clone child_stack not honored (status=0x%x)\n", status);
        return 3;
    }
    puts("PASS clone child_stack");
    return 0;
}
