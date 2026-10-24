// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest fixture: exercise the syscalls HYPERION implements in its
// compatibility-adapter layer, and report one line per group.
//
// Every check is behavioural. Nothing here inspects HYPERION's internals: the
// program is an ordinary statically linked x86-64 binary, and the same binary is
// run natively (where all checks pass) and under HYPERION.
//
// Output is deterministic so the shell test can compare it exactly: descriptor
// numbers are never printed, because they depend on how many descriptors the
// C library already holds open.
/* memfd_create and mkdtemp are POSIX/GNU extensions. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sys/times.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;

static void check(const char* name, int ok) {
    printf("%-22s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) {
        ++failures;
    }
}

int main(void) {
    /* --- descriptor-backed event sources ---------------------------------- */
    int efd = eventfd(0, EFD_NONBLOCK);
    check("eventfd", efd >= 0);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    check("epoll_create1", ep >= 0);

    int added = 0;
    if (efd >= 0 && ep >= 0) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events = EPOLLIN;
        /* 64-bit tag chosen so a truncated or misaligned copy is obvious. */
        ev.data.u64 = 0x1122334455667788ULL;
        added = epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0;
    }
    check("epoll_ctl", added);

    int ready = -1;
    unsigned long long tag = 0;
    if (added) {
        unsigned long long one = 1;
        if (write(efd, &one, sizeof one) == (ssize_t)sizeof one) {
            struct epoll_event out;
            memset(&out, 0, sizeof out);
            ready = epoll_wait(ep, &out, 1, 100);
            tag = out.data.u64;
        }
    }
    check("epoll_wait", ready == 1);
    check("epoll_event_layout", tag == 0x1122334455667788ULL);

    int polled = -1;
    short revents = 0;
    if (efd >= 0) {
        struct pollfd pfd;
        pfd.fd = efd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        polled = poll(&pfd, 1, 0);
        revents = pfd.revents;
    }
    check("poll", polled == 1);
    check("poll_revents", (revents & POLLIN) != 0);

    int selected = -1;
    int isset = 0;
    if (efd >= 0 && efd < FD_SETSIZE) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(efd, &set);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        selected = select(efd + 1, &set, NULL, NULL, &tv);
        isset = FD_ISSET(efd, &set) != 0;
    }
    check("select", selected == 1);
    check("select_fd_set", isset);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    check("timerfd_create", tfd >= 0);
    int armed = 0;
    if (tfd >= 0) {
        struct itimerspec spec;
        memset(&spec, 0, sizeof spec);
        spec.it_value.tv_sec = 1000;
        armed = timerfd_settime(tfd, 0, &spec, NULL) == 0;
    }
    check("timerfd_settime", armed);

    int memfd = memfd_create("hyperion-fixture", 0);
    check("memfd_create", memfd >= 0);

    /* --- metadata, files and directories ---------------------------------- */
    struct rusage usage;
    memset(&usage, 0, sizeof usage);
    check("getrusage", getrusage(RUSAGE_SELF, &usage) == 0);

    struct tms tms;
    memset(&tms, 0, sizeof tms);
    check("times", times(&tms) != (clock_t)-1);

    struct rlimit limits;
    check("getrlimit", getrlimit(RLIMIT_NOFILE, &limits) == 0);
    check("setrlimit", setrlimit(RLIMIT_NOFILE, &limits) == 0);

    const mode_t previous = umask(0022);
    check("umask", umask(previous) == 0022);

    char directory[] = "/tmp/hyperion-guest-XXXXXX";
    const char* made = mkdtemp(directory);
    check("mkdtemp", made != NULL);

    if (made != NULL) {
        char file[512];
        snprintf(file, sizeof file, "%s/data", made);
        int fd = open(file, O_CREAT | O_RDWR, 0644);
        int truncated = 0;
        if (fd >= 0) {
            (void)write(fd, "0123456789", 10);
            truncated = ftruncate(fd, 4) == 0;
            struct stat info;
            truncated = truncated && fstat(fd, &info) == 0 && info.st_size == 4;
            truncated = truncated && fsync(fd) == 0;
            close(fd);
        }
        check("ftruncate+fsync", truncated);

        char linkpath[512];
        char target[512];
        int linked = 0;
        snprintf(linkpath, sizeof linkpath, "%s/link", made);
        snprintf(target, sizeof target, "%s/data", made);
        if (symlink(target, linkpath) == 0) {
            char buffer[512];
            const ssize_t n = readlink(linkpath, buffer, sizeof buffer - 1);
            linked = n > 0;
            if (n > 0) {
                buffer[n] = '\0';
                linked = strcmp(buffer, target) == 0;
            }
        }
        check("symlink/readlink", linked);

        char hard[512];
        snprintf(hard, sizeof hard, "%s/hard", made);
        check("link", link(target, hard) == 0);
        check("unlink", unlink(hard) == 0 && unlink(linkpath) == 0 && unlink(target) == 0);

        check("chdir", chdir(directory) == 0);
        char cwd[512];
        check("getcwd", getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, directory) == 0);
        (void)chdir("/");
        (void)rmdir(directory);
    }

    /* --- time ------------------------------------------------------------- */
    /* nanosleep must really wait. A translator that stubs it out turns every
     * retry loop in the guest into a spin, which looks like a hang under load
     * but never fails an exit-status check. */
    struct timespec pause_req;
    pause_req.tv_sec = 0;
    pause_req.tv_nsec = 60 * 1000 * 1000; /* 60 ms */
    struct timespec before;
    struct timespec after;
    (void)clock_gettime(CLOCK_MONOTONIC, &before);
    const int slept = nanosleep(&pause_req, NULL);
    (void)clock_gettime(CLOCK_MONOTONIC, &after);
    const long long elapsed_ms =
        (long long)(after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    check("nanosleep", slept == 0 && elapsed_ms >= 50);

    struct timespec bad;
    bad.tv_sec = 0;
    bad.tv_nsec = 1000000000; /* the kernel rejects 1e9 rather than normalising */
    check("nanosleep_einval", nanosleep(&bad, NULL) == -1 && errno == EINVAL);

    /* --- process and scheduling ------------------------------------------- */
    check("sched_yield", sched_yield() == 0);
    /* MEMBARRIER_CMD_QUERY. A kernel answers with the command mask; HYPERION
     * answers 0 (it advertises no optional commands) and accepts the expedited
     * barrier as a no-op, because the guest runs on one host thread. Both are a
     * non-negative answer, which is what a caller can rely on. */
    check("membarrier", syscall(324, 0, 0) >= 0);
    check("setsid_or_pgid", getpgrp() > 0);

    printf("failures %d\n", failures);
    return failures == 0 ? 0 : 1;
}
