#include "galaxy/coop_scheduler.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <xmmintrin.h>

#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace galaxy::coop {
namespace {

// DefaultSwitchThreadCallback 0x804AAC08 is an empty guest function.
constexpr std::uint32_t kDefaultSwitchThreadCallback = 0x804AAC08u;

// A translated thread's native frames: deep scene traversals stay well below
// this. The whole stack is committed up front (nothing grows it on demand: the
// guard page below it is no-access), so a parked stack is plain memory; pages
// nobody touches cost commit charge only. The boot thread gets what the
// simulation thread's own stack had (the /STACK reserve in CMakeLists.txt):
// cooperative frames nest deeply on it.
constexpr SIZE_T kFiberStack = 16u * 1024u * 1024u;
constexpr SIZE_T kBootStack = 256u * 1024u * 1024u;
constexpr SIZE_T kGuardPage = 4096u;
// coop_context_x64.asm's frame: 0x100 bytes of saved state below the return address.
constexpr std::size_t kContextFrame = 0x100u;

extern "C" void coop_context_switch(void** save_sp, void* load_sp);
extern "C" void coop_context_trampoline();

// The stack pointer of a context that has not run yet: coop_context_switch's frame laid out so that its return
// enters coop_context_trampoline, which calls entry(argument).
void* coop_context_make(std::byte* base, std::byte* top, void (*entry)(void*), void* argument, unsigned mxcsr) {
    auto* ret = reinterpret_cast<std::uint64_t*>((reinterpret_cast<std::uintptr_t>(top) & ~std::uintptr_t{15}) - 16u);
    auto* frame = reinterpret_cast<std::byte*>(ret) - kContextFrame;
    std::memset(frame, 0, kContextFrame);
    std::uint32_t mx = mxcsr;
    std::memcpy(frame + 0xA0, &mx, sizeof(mx));
    const std::uint16_t fpcw = 0x027Fu;
    std::memcpy(frame + 0xA4, &fpcw, sizeof(fpcw));
    const std::uint64_t words[] = {
        reinterpret_cast<std::uint64_t>(base),                // +A8 DeallocationStack
        reinterpret_cast<std::uint64_t>(base + kGuardPage),   // +B0 StackLimit
        reinterpret_cast<std::uint64_t>(top),                 // +B8 StackBase
        0u, 0u, 0u, 0u,                                       // +C0 rbp rbx rdi rsi
        reinterpret_cast<std::uint64_t>(entry),               // +E0 r12
        reinterpret_cast<std::uint64_t>(argument),            // +E8 r13
        0u, 0u,                                               // +F0 r14 r15
    };
    std::memcpy(frame + 0xA8, words, sizeof(words));
    *ret = reinterpret_cast<std::uint64_t>(&coop_context_trampoline);
    return frame;
}

std::uint32_t leading_zeros(std::uint32_t value) noexcept {
    return static_cast<std::uint32_t>(std::countl_zero(value));
}

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

struct FiberStart {
    Scheduler* scheduler;
    std::uint32_t thread;
};

}  // namespace

void save_thread_registers(const PpcContext& context, ThreadRegisters& out) noexcept {
    std::memcpy(out.gpr, context.gpr, sizeof(out.gpr));
    std::memcpy(out.fpr_bits, context.fpr_bits, sizeof(out.fpr_bits));
    std::memcpy(out.ps1_bits, context.ps1_bits, sizeof(out.ps1_bits));
    out.cr = context.cr;
    out.lr = context.lr;
    out.ctr = context.ctr;
    out.xer = context.xer;
    out.fpscr = context.fpscr;
    std::memcpy(out.gqr, context.gqr, sizeof(out.gqr));
    out.msr = context.msr;
    out.pc = context.pc;
    out.reserved_address = context.reserved_address;
    out.reserved_value = context.reserved_value;
}

void load_thread_registers(const ThreadRegisters& in, PpcContext& context) noexcept {
    std::memcpy(context.gpr, in.gpr, sizeof(in.gpr));
    std::memcpy(context.fpr_bits, in.fpr_bits, sizeof(in.fpr_bits));
    std::memcpy(context.ps1_bits, in.ps1_bits, sizeof(in.ps1_bits));
    context.cr = in.cr;
    context.lr = in.lr;
    context.ctr = in.ctr;
    context.xer = in.xer;
    context.fpscr = in.fpscr;
    std::memcpy(context.gqr, in.gqr, sizeof(in.gqr));
    context.msr = in.msr;
    context.pc = in.pc;
    context.reserved_address = in.reserved_address;
    context.reserved_value = in.reserved_value;
}

std::uint32_t GuestRam::load32(std::uint32_t address) const noexcept {
    std::uint32_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return byte_swap_u32(value);
}

std::uint16_t GuestRam::load16(std::uint32_t address) const noexcept {
    std::uint16_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return byte_swap_u16(value);
}

std::uint64_t GuestRam::load64(std::uint32_t address) const noexcept {
    std::uint64_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return byte_swap_u64(value);
}

void GuestRam::store32(std::uint32_t address, std::uint32_t value) const noexcept {
    const std::uint32_t swapped = byte_swap_u32(value);
    std::memcpy(base + address, &swapped, sizeof(swapped));
}

void GuestRam::store16(std::uint32_t address, std::uint16_t value) const noexcept {
    const std::uint16_t swapped = byte_swap_u16(value);
    std::memcpy(base + address, &swapped, sizeof(swapped));
}

Scheduler::Scheduler(Host host, GuestRam ram, PpcContext* shared_context)
    : host_(host), ram_(ram), context_(shared_context) {
    if (host_.run_new_thread == nullptr || host_.idle == nullptr ||
        host_.call_guest == nullptr || host_.exchange_chain == nullptr ||
        ram_.base == nullptr || context_ == nullptr) {
        throw std::logic_error("cooperative scheduler needs every host hook");
    }
}

Scheduler::~Scheduler() {
    // Only after the boot fiber has ended (run_boot returned): no stack is in use by the caller.
    for (Stack& stack : stacks_) {
        if (stack.base != nullptr) VirtualFree(stack.base, 0, MEM_RELEASE);
    }
}

int Scheduler::acquire_stack(std::size_t size) {
    // The lowest free slot of that size, so the same sequence of thread creations gets the same stacks.
    for (std::size_t i = 0; i < stacks_.size(); ++i) {
        if (!stacks_[i].in_use && static_cast<std::size_t>(stacks_[i].top - stacks_[i].base) == size + kGuardPage) {
            stacks_[i].in_use = true;
            return static_cast<int>(i);
        }
    }
    auto* base = static_cast<std::byte*>(
        VirtualAlloc(nullptr, size + kGuardPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (base == nullptr) {
        throw std::runtime_error("cannot allocate a guest thread stack: " + std::to_string(GetLastError()));
    }
    DWORD old = 0;
    VirtualProtect(base, kGuardPage, PAGE_NOACCESS, &old);
    stacks_.push_back(Stack{base, base + kGuardPage + size, nullptr, true});
    return static_cast<int>(stacks_.size() - 1u);
}

void Scheduler::switch_context(int to_slot) {
    void** const save = current_slot_ == kNoSlot ? &host_sp_ : &stacks_[current_slot_].sp;
    void* const load = to_slot == kNoSlot ? host_sp_ : stacks_[to_slot].sp;
    current_slot_ = to_slot;
    coop_context_switch(save, load);
    // Resumed: whoever switched here set current_slot_ to this context.
}

void Scheduler::boot_entry(void* parameter) {
    auto* scheduler = static_cast<Scheduler*>(parameter);
    try {
        scheduler->boot_function_(scheduler->boot_argument_);
    } catch (...) {
        scheduler->boot_result_ = std::current_exception();
    }
    // Back to the simulation thread's context for good; this stack is never resumed.
    scheduler->switch_context(kNoSlot);
    std::terminate();
}

void Scheduler::run_boot(void (*entry)(void*), void* argument) {
    if (boot_slot_ != kNoSlot) throw std::logic_error("the cooperative boot fiber runs once");
    // New fibers start from the simulation thread's float mode (flush-to-zero, rounding).
    mxcsr_ = _mm_getcsr();
    boot_function_ = entry;
    boot_argument_ = argument;
    boot_slot_ = acquire_stack(kBootStack);
    Stack& stack = stacks_[boot_slot_];
    stack.sp = coop_context_make(stack.base, stack.top, &Scheduler::boot_entry, this, mxcsr_);
    int resume = boot_slot_;
    for (;;) {
        switch_context(resume);
        // Back on this context: a fiber asks for a snapshot or a restore, or the boot fiber has ended.
        const HostRequest request = host_request_;
        host_request_ = HostRequest::None;
        if (request == HostRequest::Save) {
            host_save(*save_target_);
            resume = requester_slot_;
        } else if (request == HostRequest::Restore) {
            resume = host_restore(*restore_source_);
        } else {
            break;
        }
    }
    if (boot_result_ != nullptr) {
        std::exception_ptr result = boot_result_;
        boot_result_ = nullptr;
        std::rethrow_exception(result);
    }
}

bool Scheduler::save(Snapshot& out) {
    if (current_slot_ == kNoSlot) throw std::logic_error("cooperative snapshot outside a fiber");
    host_request_ = HostRequest::Save;
    requester_slot_ = current_slot_;
    save_target_ = &out;
    resumed_by_restore_ = false;
    switch_context(kNoSlot);
    // Resumed after the capture, or by restore() (which set resumed_by_restore_).
    const bool restored = resumed_by_restore_;
    resumed_by_restore_ = false;
    return restored;
}

void Scheduler::restore(const Snapshot& in) {
    if (current_slot_ == kNoSlot || !in.valid) throw std::logic_error("cooperative restore without a snapshot");
    host_request_ = HostRequest::Restore;
    restore_source_ = &in;
    switch_context(kNoSlot);
    std::terminate();  // the context that called restore is gone
}

void Scheduler::host_save(Snapshot& out) {
    out.stacks.clear();
    for (std::size_t i = 0; i < stacks_.size(); ++i) {
        const Stack& stack = stacks_[i];
        if (!stack.in_use || stack.sp == nullptr) continue;
        Snapshot::StackImage image;
        image.slot = static_cast<int>(i);
        image.sp = stack.sp;
        const auto* from = static_cast<const std::byte*>(stack.sp);
        image.bytes.assign(from, static_cast<const std::byte*>(stack.top));
        out.stacks.push_back(std::move(image));
    }
    out.fibers = fibers_;
    out.current_slot = requester_slot_;
    out.running_thread = running_thread_;
    out.boot_thread = boot_thread_;
    out.unowned_chain = unowned_chain_;
    out.retired = retired_;
    out.switches = switches_;
    out.fibers_created = fibers_created_;
    out.idle_entries = idle_entries_;
    out.idle_passes = idle_passes_;
    out.valid = true;
}

int Scheduler::host_restore(const Snapshot& in) {
    // Every stack the snapshot knew is back in use with its bytes; a stack taken since is free again (it is
    // still allocated, so slots keep their addresses).
    for (Stack& stack : stacks_) {
        stack.in_use = false;
        stack.sp = nullptr;
    }
    for (const Snapshot::StackImage& image : in.stacks) {
        Stack& stack = stacks_[static_cast<std::size_t>(image.slot)];
        stack.in_use = true;
        stack.sp = image.sp;
        std::memcpy(image.sp, image.bytes.data(), image.bytes.size());
    }
    stacks_[static_cast<std::size_t>(boot_slot_)].in_use = true;
    fibers_ = in.fibers;
    running_thread_ = in.running_thread;
    boot_thread_ = in.boot_thread;
    unowned_chain_ = in.unowned_chain;
    retired_ = in.retired;
    switches_ = in.switches;
    fibers_created_ = in.fibers_created;
    idle_entries_ = in.idle_entries;
    idle_passes_ = in.idle_passes;
    fiber_error_ = nullptr;
    resumed_by_restore_ = true;
    return in.current_slot;
}

ChainState& Scheduler::running_chain() noexcept {
    if (running_thread_ != 0u) {
        const auto it = fibers_.find(running_thread_);
        if (it != fibers_.end() && it->second.slot == current_slot_ && current_slot_ != kNoSlot) {
            return it->second.chain;
        }
    }
    return unowned_chain_;
}

void Scheduler::set_current(std::uint32_t thread, std::uint32_t context_address) noexcept {
    ram_.store32(os::kCurrentThread, thread);
    ram_.store32(os::kCurrentContext, context_address);
    ram_.store32(os::kCurrentContextPhysical, context_address & 0x3FFFFFFFu);
    if (context_address != 0u && context_address != os::kIdleContext) {
        // Every fiber keeps its whole register file, so the FPU always
        // belongs to the running context: the SDK's lazy owner switch never
        // has anything to save or load.
        ram_.store32(os::kFpuContext, context_address);
        context_->msr |= os::kMsrFloatingPoint;
    }
}

void Scheduler::call_switch_callback(std::uint32_t old_thread, std::uint32_t new_thread) {
    const std::uint32_t callback = ram_.load32(os::kSwitchThreadCallback);
    if (callback == 0u || callback == kDefaultSwitchThreadCallback) return;
    // The SDK calls it from SelectThread on the outgoing thread's stack with
    // interrupts disabled; the caller's registers survive the call.
    ThreadRegisters saved;
    save_thread_registers(*context_, saved);
    context_->gpr[3] = old_thread;
    context_->gpr[4] = new_thread;
    context_->gpr[1] -= 0x40u;
    host_.call_guest(host_.user, callback, context_);
    load_thread_registers(saved, *context_);
}

void Scheduler::enqueue_run(std::uint32_t thread) noexcept {
    // SDK SetRun: tail-enqueue on RunQueue[priority], mark the priority bit.
    const std::uint32_t priority = ram_.load32(thread + os::kThreadPriority) & 31u;
    const std::uint32_t queue = os::kRunQueue + priority * 8u;
    ram_.store32(thread + os::kThreadQueue, queue);
    const std::uint32_t tail = ram_.load32(queue + 4u);
    if (tail == 0u) {
        ram_.store32(queue, thread);
    } else {
        ram_.store32(tail + os::kThreadLinkNext, thread);
    }
    ram_.store32(thread + os::kThreadLinkPrev, tail);
    ram_.store32(thread + os::kThreadLinkNext, 0u);
    ram_.store32(queue + 4u, thread);
    ram_.store32(os::kRunQueueBits,
                 ram_.load32(os::kRunQueueBits) | (1u << (31u - priority)));
    ram_.store32(os::kRunQueueHint, 1u);
}

void Scheduler::select_thread(bool yield) {
    context_->gpr[3] = 0u;
    if (static_cast<std::int32_t>(ram_.load32(os::kReschedule)) > 0) return;

    const std::uint32_t current_context = ram_.load32(os::kCurrentContext);
    const std::uint32_t current = ram_.load32(os::kCurrentThread);
    if (current_context != current) return;

    if (current != 0u) {
        // The fiber executing SelectThread belongs to the current thread.
        Fiber& self = fibers_[current];
        if (self.slot == kNoSlot) {
            self.slot = current_slot_;
            self.started = true;
            self.boot = self.slot == boot_slot_;
            if (self.boot) {
                boot_thread_ = current;
            }
        } else if (self.slot != current_slot_) {
            throw std::runtime_error(
                "cooperative SelectThread: current thread " + hex(current) +
                " belongs to another fiber");
        }
        running_thread_ = current;
        if (ram_.load16(current + os::kThreadState) == os::kStateRunning) {
            if (!yield) {
                const std::uint32_t bits = ram_.load32(os::kRunQueueBits);
                const std::int32_t highest = static_cast<std::int32_t>(leading_zeros(bits));
                if (static_cast<std::int32_t>(ram_.load32(current + os::kThreadPriority)) <= highest) {
                    return;
                }
            }
            ram_.store16(current + os::kThreadState, os::kStateReady);
            enqueue_run(current);
        }
    }

    if (ram_.load32(os::kRunQueueBits) == 0u) {
        call_switch_callback(current, 0u);
        set_current(0u, os::kIdleContext);
        ++idle_entries_;
        do {
            ++idle_passes_;
            host_.idle(host_.user, context_);
        } while (ram_.load32(os::kRunQueueBits) == 0u);
    }

    ram_.store32(os::kRunQueueHint, 0u);
    const std::uint32_t bits = ram_.load32(os::kRunQueueBits);
    const std::uint32_t priority = leading_zeros(bits);
    const std::uint32_t queue = os::kRunQueue + priority * 8u;
    const std::uint32_t next = ram_.load32(queue);
    if (next == 0u) {
        throw std::runtime_error(
            "cooperative SelectThread: run queue " + std::to_string(priority) +
            " is marked ready but empty");
    }
    const std::uint32_t after = ram_.load32(next + os::kThreadLinkNext);
    if (after == 0u) {
        ram_.store32(queue + 4u, 0u);
    } else {
        ram_.store32(after + os::kThreadLinkPrev, 0u);
    }
    ram_.store32(queue, after);
    if (after == 0u) {
        ram_.store32(os::kRunQueueBits, bits & ~(1u << (31u - priority)));
    }
    ram_.store32(next + os::kThreadQueue, 0u);
    ram_.store16(next + os::kThreadState, os::kStateRunning);

    // __OSSwitchThread: OSSetCurrentThread (switch callback), then the
    // context; the "load" is the fiber switch below.
    call_switch_callback(ram_.load32(os::kCurrentThread), next);
    set_current(next, next);
    if (next != running_thread_ || fibers_[next].slot != current_slot_) {
        switch_to(next);
    }
}

void Scheduler::switch_to(std::uint32_t next) {
    ChainState* const parked = &running_chain();
    if (running_thread_ != 0u) {
        Fiber& self = fibers_[running_thread_];
        if (self.slot == current_slot_ && current_slot_ != kNoSlot) {
            save_thread_registers(*context_, self.saved);
            self.started = true;
        }
    }
    // References into fibers_ stay valid across insertion.
    Fiber& target = fibers_[next];
    if (target.slot == kNoSlot) {
        auto* start = new FiberStart{this, next};
        target.slot = acquire_stack(kFiberStack);
        Stack& stack = stacks_[target.slot];
        stack.sp = coop_context_make(stack.base, stack.top, &Scheduler::fiber_entry, start, mxcsr_);
        target.started = false;
        target.chain = {};
        ++fibers_created_;
    } else if (target.started) {
        load_thread_registers(target.saved, *context_);
    }
    host_.exchange_chain(host_.user, *parked, target.chain);
    running_thread_ = next;
    ++switches_;
    switch_context(target.slot);

    // Resumed: the switching fiber restored this thread's registers and chain.
    reap_exited();
    release_parked();
    if (fiber_error_ != nullptr && current_slot_ == boot_slot_) {
        std::exception_ptr error = fiber_error_;
        fiber_error_ = nullptr;
        std::rethrow_exception(error);
    }
}

void Scheduler::return_to_boot() noexcept {
    ChainState& parked = running_chain();
    const auto boot = fibers_.find(boot_thread_);
    const ChainState& resumed =
        boot != fibers_.end() && boot->second.slot == boot_slot_
            ? boot->second.chain
            : unowned_chain_;
    host_.exchange_chain(host_.user, parked, resumed);
    running_thread_ = boot_thread_;
    switch_context(boot_slot_);
    // The boot fiber never resumes a fiber that reported a failure.
    std::terminate();
}

void Scheduler::fiber_entry(void* parameter) {
    auto* start = static_cast<FiberStart*>(parameter);
    Scheduler* const scheduler = start->scheduler;
    const std::uint32_t thread = start->thread;
    delete start;
    _mm_setcsr(scheduler->mxcsr_);
    try {
        scheduler->host_.run_new_thread(scheduler->host_.user, thread, scheduler->context_);
        throw std::runtime_error(
            "guest thread " + hex(thread) + " fiber resumed after its exit");
    } catch (...) {
        scheduler->fiber_error_ = std::current_exception();
    }
    // Report on the boot fiber, which owns the runtime's top-level handler.
    scheduler->return_to_boot();
}

void Scheduler::on_thread_created(std::uint32_t thread) {
    const auto it = fibers_.find(thread);
    if (it == fibers_.end()) return;
    if (it->second.slot != kNoSlot && !it->second.boot) {
        // Its own fiber (current) keeps running on the stack until it switches away.
        retired_.push_back(it->second.slot);
    }
    fibers_.erase(it);
    release_parked();
}

// A thread that exited or was cancelled (state 0, or MORIBUND until joined)
// runs again only through OSCreateThread, which starts a new fiber: its parked
// fiber is released.
void Scheduler::reap_exited() {
    for (auto it = fibers_.begin(); it != fibers_.end();) {
        const Fiber& fiber = it->second;
        const std::uint16_t state = ram_.load16(it->first + os::kThreadState);
        if (fiber.slot != kNoSlot && fiber.slot != current_slot_ && !fiber.boot &&
            fiber.started && (state == 0u || state == os::kStateMoribund)) {
            retired_.push_back(fiber.slot);
            it = fibers_.erase(it);
        } else {
            ++it;
        }
    }
}

void Scheduler::release_parked() {
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (*it != current_slot_) {
            stacks_[*it].in_use = false;
            stacks_[*it].sp = nullptr;
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace galaxy::coop
