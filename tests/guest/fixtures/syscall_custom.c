// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: call a syscall number HYPERION does not implement statically,
// so the only way it can succeed is through a rule in the persistent registry.
//
// 300 is unassigned on x86-64. The test's registry adds
//   { "syscall": 300, "implementation": "guest_alias", "target_syscall": 39 }
// which maps it onto getpid(2). Nothing in HYPERION's binary knows about 300, so
// a successful call proves the JSON rule reached the dispatch table.
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    errno = 0;
    const long result = syscall(300);
    if (result > 0) {
        /* getpid() is always positive; the value itself is not deterministic. */
        printf("custom=OK\n");
    } else if (errno == ENOSYS || result == -ENOSYS) {
        printf("custom=ENOSYS\n");
    } else {
        printf("custom=UNEXPECTED(errno=%d,result=%ld)\n", errno, result);
    }
    return 0;
}
