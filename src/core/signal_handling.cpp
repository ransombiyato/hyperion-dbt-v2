// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest signal delivery. See include/hyperion/signal_handling.hpp for the
// design and constraints.
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

#include "hyperion/signal_handling.hpp"

#include "hyperion/syscall.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace hyperion {
namespace {

// Guest signals queued by the host signal handler (async-signal-safe). Bit
// (sig - 1) matches the guest's sigset_t layout.
std::atomic<std::uint64_t> g_pending_host_signals{0};

// Async-signal-safe handler: record the signal and nothing else.
void host_signal_handler(int sig, siginfo_t*, void*) {
    if (sig >= 1 && sig <= 64) {
        g_pending_host_signals.fetch_or(1ULL << (sig - 1),
                                        std::memory_order_relaxed);
    }
}

bool install_one(int sig) {
    struct sigaction sa{};
    sa.sa_sigaction = &host_signal_handler;
    // Do NOT set SA_RESTART: we want blocking guest syscalls (read, poll, ...)
    // to be interrupted with EINTR so the guest can observe the signal and the
    // runtime can deliver it to a guest handler at the next instruction boundary.
    sa.sa_flags = SA_SIGINFO;
    if (::sigemptyset(&sa.sa_mask) != 0) {
        return false;
    }
    return ::sigaction(sig, &sa, nullptr) == 0;
}

// Clear the host-queued bit for a signal once it has been taken into account.
void clear_host_signal(int sig) {
    if (sig >= 1 && sig <= 64) {
        g_pending_host_signals.fetch_and(~(1ULL << (sig - 1)),
                                         std::memory_order_relaxed);
    }
}

// Write a little-endian u64/u32 into a byte buffer.
void put64(std::uint8_t* buf, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}
void put32(std::uint8_t* buf, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        buf[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

// Highest-priority pending unblocked signal (lowest number first, matching the
// kernel's order for signal delivery). Returns 0 when nothing should be
// delivered right now.
int pick_signal(const SyscallHandler& handler) {
    std::uint64_t host_mask = g_pending_host_signals.load(std::memory_order_relaxed);
    std::uint64_t pending = host_mask | handler.signal_pending_for_delivery();
    pending &= ~handler.guest_sigmask(); // remove blocked signals
    for (int sig = 1; sig <= 64; ++sig) {
        const std::uint64_t bit = 1ULL << (sig - 1);
        if ((pending & bit) == 0) {
            continue;
        }
        if (sig < static_cast<int>(handler.sigaction_count())) {
            const auto& act = handler.sigaction_at(static_cast<std::size_t>(sig));
            if (act.installed && act.handler == 1) {
                continue; // SIG_IGN: skip
            }
        }
        return sig;
    }
    return 0;
}

} // namespace

void install_host_signal_handlers(SyscallHandler& /*handler*/) noexcept {
    static const int kSignals[] = {
        SIGHUP,   SIGINT,   SIGQUIT,  SIGILL,   SIGTRAP,  SIGABRT,
        SIGBUS,   SIGFPE,   SIGUSR1,  SIGSEGV,  SIGUSR2,  SIGPIPE,
        SIGALRM,  SIGTERM,  SIGCHLD,  SIGCONT,  SIGTSTP,  SIGTTIN,
        SIGTTOU,  SIGWINCH, SIGURG,   SIGXCPU,  SIGXFSZ,  SIGVTALRM,
        SIGPROF,
    };
    for (int sig : kSignals) {
        (void)install_one(sig);
    }
}

void queue_host_signal(int host_signal) {
    if (host_signal >= 1 && host_signal <= 64) {
        g_pending_host_signals.fetch_or(1ULL << (host_signal - 1),
                                        std::memory_order_relaxed);
    }
}

bool signal_pending(const SyscallHandler& handler) {
    return pick_signal(handler) != 0;
}

bool check_and_deliver_signals(SyscallHandler& handler, GuestState& state,
                               Memory& memory, bool& should_exit,
                               std::int64_t& exit_code) {
    should_exit = false;
    exit_code = 0;

    int sig = pick_signal(handler);
    if (sig == 0) {
        return false; // nothing to deliver
    }

    // Dequeue the signal from both queues.
    clear_host_signal(sig);
    handler.clear_pending_signal(sig);

    const auto index = static_cast<std::size_t>(sig);
    if (index < handler.sigaction_count() &&
        handler.sigaction_at(index).installed) {
        const auto& act = handler.sigaction_at(index);
        if (act.handler == 1) {
            // SIG_IGN: already filtered by pick_signal, but be safe.
            return false;
        }
        if (act.handler == 0) {
            // SIG_DFL: apply the default action.
            switch (sig) {
            case 17: // SIGCHLD
            case 18: // SIGCONT
            case 19: // SIGSTOP
            case 20: // SIGTSTP
            case 21: // SIGTTIN
            case 22: // SIGTTOU
            case 23: // SIGURG
            case 28: // SIGWINCH
                return false; // ignored by default
            default:
                should_exit = true;
                exit_code = 128 + sig;
                return true;
            }
        }
        // A real handler address is installed: build an x86-64 rt_sigframe and
        // divert to it. After the handler runs, rt_sigreturn(15) restores state.
        // The stack layout is:
        //   [frame_addr + 0] = kSigReturnMagic (for plain `ret` interception)
        //   [frame_addr + 8] = kSigReturnMagic (for `leave; ret` interception)
        //   [frame_addr + 16] = ucontext header (uc_flags, uc_link, ...)
        //   [frame_addr + 16+64] = sigcontext (saved guest registers)
        //   [frame_addr + 256] = siginfo_t
        // rsp = frame_addr so the handler's `ret` pops kSigReturnMagic and the
        // execution loop intercepts it to invoke rt_sigreturn.

        // Align the guest stack down to 16 bytes (x86-64 ABI requirement).
        std::uint64_t new_rsp = state.reg(Gpr::kRsp) & ~0xFULL;
        const std::uint64_t frame_addr = new_rsp - sigframe::kFrameSize;

        std::uint8_t frame[sigframe::kFrameSize];
        std::memset(frame, 0, sizeof(frame));

        // Return address: the handler's `ret` pops this; the exec loop
        // intercepts kSigReturnMagic and invokes rt_sigreturn. Written at both
        // offset 0 (plain `ret`) and offset 8 (`leave; ret`: `pop rbp` consumes
        // offset 0, `ret` consumes offset 8 into rip).
        put64(frame, 0, sigframe::kSigReturnMagic);
        put64(frame, 8, sigframe::kSigReturnMagic);

        // ucontext at kUcontextOffset (= 16) within the frame.
        const std::size_t uc = sigframe::kUcontextOffset;
        put64(frame, uc + sigframe::kUcFlags, 0);            // uc_flags
        put64(frame, uc + sigframe::kUcLink, 0);             // uc_link = NULL
        // uc_stack: ss_sp=0, ss_flags=SS_DISABLE(2), ss_size=0
        put64(frame, uc + sigframe::kUcStack + 0, 0);
        put32(frame, uc + sigframe::kUcStack + 8, 2);
        put64(frame, uc + sigframe::kUcStack + 16, 0);
        // uc_sigmask: current guest mask (the delivered signal is added on
        // rt_sigreturn via the handler's own mask tracking).
        put64(frame, uc + sigframe::kUcSigmask + 0, handler.guest_sigmask());
        put64(frame, uc + sigframe::kUcSigmask + 8, 0);

        // uc_mcontext = struct sigcontext: save all guest registers.
        const std::size_t mc = uc + sigframe::kUcMcontext;
        put64(frame, mc + 0,   state.reg(Gpr::kR8));
        put64(frame, mc + 8,   state.reg(Gpr::kR9));
        put64(frame, mc + 16,  state.reg(Gpr::kR10));
        put64(frame, mc + 24,  state.reg(Gpr::kR11));
        put64(frame, mc + 32,  state.reg(Gpr::kR12));
        put64(frame, mc + 40,  state.reg(Gpr::kR13));
        put64(frame, mc + 48,  state.reg(Gpr::kR14));
        put64(frame, mc + 56,  state.reg(Gpr::kR15));
        put64(frame, mc + 64,  state.reg(Gpr::kRdi));
        put64(frame, mc + 72,  state.reg(Gpr::kRsi));
        put64(frame, mc + 80,  state.reg(Gpr::kRbp));
        put64(frame, mc + 88,  state.reg(Gpr::kRbx));
        put64(frame, mc + 96,  state.reg(Gpr::kRdx));
        put64(frame, mc + 104, state.reg(Gpr::kRax));
        put64(frame, mc + 112, state.reg(Gpr::kRcx));
        put64(frame, mc + 120, state.reg(Gpr::kRsp)); // pre-signal stack pointer
        put64(frame, mc + 128, state.rip);
        put64(frame, mc + 136, 0x0202ULL);            // eflags (bit 1 always set)
        put64(frame, mc + 144, 0x33ULL);              // cs
        put64(frame, mc + 152, 0ULL);                 // gs
        put64(frame, mc + 160, state.fs_base);        // fs
        put64(frame, mc + 168, 0x2bULL);              // ss

        // siginfo_t at kSiginfoOffset.
        const std::size_t si = sigframe::kSiginfoOffset;
        put32(frame, si + sigframe::kSiSigno, static_cast<std::uint32_t>(sig));
        put32(frame, si + sigframe::kSiErrno, 0);
        put32(frame, si + sigframe::kSiCode, 0); // SI_USER

        // Write the frame to guest memory.
        if (!memory.write(frame_addr, frame, sizeof(frame))) {
            return false; // cannot deliver: treat as ignored
        }

        // Map an alias at the zero-extended address of the frame page. Without
        // this, 32-bit register operations in the signal handler (e.g. `mov
        // ebp, esp`) zero-extend `esp` to the low 4GB range, which is not
        // mapped as stack memory, causing faults on every stack access.
        const std::uint64_t orig_page = frame_addr & ~0xFFFULL;
        const std::uint64_t alias_page = (frame_addr & 0xFFFFFFFFULL) & ~0xFFFULL;
        if (alias_page != orig_page) {
            (void)memory.map_page_alias(orig_page, alias_page);
        }

        // Set up guest state for the handler call:
        //   rdi = sig
        //   rsi = &sigframe->info
        //   rdx = &sigframe->uc
        //   rip = handler address
        //   rsp = points at the return address (start of frame)
        state.reg(Gpr::kRdi) = static_cast<std::uint64_t>(sig);
        state.reg(Gpr::kRsi) = frame_addr + sigframe::kSiginfoOffset;
        state.reg(Gpr::kRdx) = frame_addr + sigframe::kUcontextOffset;
        state.reg(Gpr::kRsp) = frame_addr;
        state.rip = act.handler;
        state.flags = {};
        state.cc_op = 0;
        state.pf_af_pending = 0;

        return true; // signal delivered; guest will run the handler
    }

    // No handler installed and not ignored: terminate-class default.
    should_exit = true;
    exit_code = 128 + sig;
    return true;
}

} // namespace hyperion
