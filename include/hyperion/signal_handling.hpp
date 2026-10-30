// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest signal delivery for HPN.
//
// The mechanism: host signals are trapped with sigaction(SA_SIGINFO) on the
// single host thread. The handler records the signal in a pending-signal
// bitmask (async-signal-safe: only an atomic store). At each guest instruction
// boundary, check_and_deliver_signals() picks the highest-priority pending
// unblocked signal and, if the guest installed a handler, builds an x86-64
// rt_sigframe on the guest stack and diverts rip to the handler. The handler
// eventually calls rt_sigreturn(15) (implemented in src/core/syscall.cpp) to
// restore the interrupted state.
//
// No host thread is created per guest thread, no ptrace is used, and no
// /dev/kvm is required: this works on Android/Termux exactly as on desktop
// Linux, because it uses only sigaction(2) and ordinary memory stores.
//
// The syscall interface (SyscallHandler base class) provides default no-op
// signal methods. MinimalSyscallHandler overrides them with real delivery.

#ifndef HYPERION_SIGNAL_HANDLING_HPP
#define HYPERION_SIGNAL_HANDLING_HPP

#include <cstdint>
#include <sys/types.h>

#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"

namespace hyperion {

// x86-64 `rt_sigframe` layout (return address + ucontext + sigcontext + siginfo).
// The layout is HPN's own: it only needs to be consistent between
// check_and_deliver_signals() (which writes it) and rt_sigreturn (which reads
// it). It does not have to match the kernel byte-for-byte.
//
// Frame layout (all offsets relative to frame_addr):
//   +0     : return address = kSigReturnMagic (for plain `ret` interception)
//   +8     : return address = kSigReturnMagic (for `leave; ret` interception:
//            `mov rsp, rbp` sets rsp to frame_addr, `pop rbp` consumes +0,
//            `ret` consumes +8 into rip)
//   +16    : ucontext header starts here
//   +16+0  : uc_flags
//   +16+8  : uc_link
//   +16+16 : uc_stack (stack_t, 24 bytes)
//   +16+40 : padding (8 bytes)
//   +16+48 : uc_sigmask (sigset_t, 16 bytes)
//   +16+64 : struct sigcontext (176 bytes: 22 x 8-byte fields)
//   +16+64+176 = +256: siginfo_t (128 bytes)
//   +16+64+176+128 = +384: end of frame
namespace sigframe {
inline constexpr std::size_t kUcontextOffset = 16;  // ucontext starts after two magic slots
inline constexpr std::size_t kSiginfoOffset = 256;  // siginfo starts after ucontext+sigcontext
inline constexpr std::size_t kFrameSize = 384;      // 16 + 64 + 176 + 128

// uc fields (relative to ucontext start at kUcontextOffset)
//   uc_flags at +0 (8 bytes)
//   uc_link at +8 (8 bytes)
//   uc_stack at +16 (24 bytes: ss_sp=16, ss_flags=24, ss_size=32) → ends at +40
//   uc_sigmask at +40 (16 bytes) → ends at +56
//   padding +56... no, mcontext starts at +56
inline constexpr std::size_t kUcFlags = 0;
inline constexpr std::size_t kUcLink = 8;
inline constexpr std::size_t kUcStack = 16;   // stack_t: ss_sp(8), ss_flags(4), ss_size(8) → 24 bytes
inline constexpr std::size_t kUcSigmask = 40; // sigset_t: 16 bytes → ends at +56
inline constexpr std::size_t kUcMcontext = 56; // struct sigcontext starts at uc+56 (= frame+72)

// siginfo_t fields (within kSiginfoOffset)
inline constexpr std::size_t kSiSigno = 0;
inline constexpr std::size_t kSiErrno = 4;
inline constexpr std::size_t kSiCode = 8;

// Magic return address placed on the guest stack as the signal handler's
// return address. When the handler executes `ret`, it pops kSigReturnMagic and the
// execution loop intercepts it to invoke rt_sigreturn to restore the interrupted
// guest context. Placed at both offset 0 (for plain `ret`) and offset 8 (for
// `leave; ret` epilogues where `mov rsp, rbp` then `pop rbp` consumes offset 0).
inline constexpr std::uint64_t kSigReturnMagic = 0xFFFFFFFFFF000000ULL;
} // namespace sigframe

// Install host signal handlers (SIGINT, SIGTERM, etc.) on the current thread,
// queueing them as guest signals so they reach the guest at the next
// instruction boundary. Call once before the guest starts running.
void install_host_signal_handlers(SyscallHandler& handler) noexcept;

// Queue a host signal as a guest signal. Called from the host signal handler
// (async-signal-safe context: only write()-safe operations).
void queue_host_signal(int host_signal);

// Return true if there is at least one unblocked guest signal pending.
bool signal_pending(const SyscallHandler& handler);

// Deliver one pending guest signal (or terminate the guest on default action).
// Returns true when a signal was delivered (state was modified) or the guest
// should exit. `should_exit` and `exit_code` are set when the default action
// terminates the guest. Safe to call at every instruction/block boundary.
//
// The handler must outlive the call; Memory must be the guest's address space.
bool check_and_deliver_signals(SyscallHandler& handler, GuestState& state,
                               Memory& memory, bool& should_exit,
                               std::int64_t& exit_code);

} // namespace hyperion

#endif // HYPERION_SIGNAL_HANDLING_HPP
