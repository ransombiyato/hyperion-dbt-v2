// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Process support shared between src/core/syscall.cpp (dispatch) and
// src/core/process.cpp (implementation).
#ifndef HYPERION_PROCESS_HPP
#define HYPERION_PROCESS_HPP

#include <cstdint>

#include "hyperion/guest_state.hpp"
#include "hyperion/syscall.hpp"

namespace hyperion::process {

// The child-side half of clone(2). fork(2) and vfork(2) pass a defaulted (empty)
// spec, since neither takes clone flags.
struct ChildCloneSpec {
    std::uint64_t flags = 0;
    std::uint64_t child_tidptr = 0; // CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID
    std::uint64_t tls = 0;          // CLONE_SETTLS
};

// Apply the child-side clone flags and then run the guest to completion in the
// *current* (already forked) host process, terminating it with the guest's exit
// status. Does not return. Called from the child branch of the fork inside
// do_clone()/do_fork(), because these flags must take effect before the guest's
// first instruction in the child executes.
[[noreturn]] void run_child_prologue(const GuestState& child_state,
                                     const GuestAddressSpace& parent_space,
                                     const std::vector<GuestDescriptor>& parent_files,
                                     const std::string& sysroot, int tier, std::uint64_t max_steps,
                                     const ChildCloneSpec& spec, int vfork_write_fd);

// fork(2): duplicate the calling guest process. Returns the child's pid in the
// parent and 0 in the child, or a negative errno.
std::int64_t do_fork(MinimalSyscallHandler& self, GuestState& state, Memory& memory);

// vfork(2): as fork(2), but the parent is suspended until the child execs or
// exits. The address space is *not* shared -- see COMPATIBILITY.md.
std::int64_t do_vfork(MinimalSyscallHandler& self, GuestState& state, Memory& memory);

// clone(2), process-creating subset. Thread-creating and namespace-creating
// flags are rejected with -ENOSYS rather than approximated.
std::int64_t do_clone(MinimalSyscallHandler& self, GuestState& state, Memory& memory);

// wait4(2): reap a child and report its status. The host's wait status word is
// already in the encoding the guest expects.
std::int64_t do_wait4(MinimalSyscallHandler& self, GuestState& state, Memory& memory);

// execve(2): replace the guest image. Non-destructive on every error path: the
// outgoing image is only discarded once the new one is known to be loadable.
std::int64_t do_execve(MinimalSyscallHandler& self, GuestState& state, Memory& memory);

} // namespace hyperion::process

#endif // HYPERION_PROCESS_HPP
