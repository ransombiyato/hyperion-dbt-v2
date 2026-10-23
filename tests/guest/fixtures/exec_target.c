// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: execve(2) target for execve_child.c. Prints its first argument
// and exits 3, so the parent can confirm both that the new image ran and that
// its exit status propagated back through waitpid(2).
#define _GNU_SOURCE // fixtures are built with a plain `cc`; be explicit
#include <stdio.h>

int main(int argc, char** argv) {
    printf("target argv1=%s\n", (argc > 1) ? argv[1] : "(none)");
    return 3;
}
