#pragma once

// Cooperative guest scheduler (GALAXY_COOP=1): every guest OSThread runs on
// its own fiber of the simulation thread, and SelectThread is host code
// that keeps the RVL SDK's run queues in guest RAM exactly as the SDK does. A
// thread switch is one context switch; nothing unwinds and nothing is resumed
// through OSLoadContext. Interrupt-level work (VI, alarms, device callbacks)
// runs as plain calls on the current fiber with the scheduler disabled, and
// the end-of-interrupt reschedule runs once the runtime callback that
// delivered it has returned, which is the SDK's __OSDispatchInterrupt order.
//
// The fibers are the scheduler's own (coop_context_x64.asm), not Windows fibers: each runs on a stack from a
// pool the scheduler allocates, slot by slot, and a parked fiber is completely described by its stack bytes and
// stack pointer. The guest's boot thread runs on one too (run_boot), so the simulation thread's own stack, with
// the runtime's state on it, is never part of a fiber. That is what lets rollback snapshot the guest threads.

#include <cstdint>
#include <exception>
#include <unordered_map>
#include <vector>

#include "galaxy/native_api.h"

namespace galaxy::coop {

// RMGE01 OS globals (RVL SDK layout).
namespace os {
inline constexpr std::uint32_t kCurrentContextPhysical = 0x800000C0u;
inline constexpr std::uint32_t kCurrentContext = 0x800000D4u;
inline constexpr std::uint32_t kFpuContext = 0x800000D8u;
inline constexpr std::uint32_t kCurrentThread = 0x800000E4u;
inline constexpr std::uint32_t kSwitchThreadCallback = 0x8069E0D8u;
inline constexpr std::uint32_t kReschedule = 0x806A2D28u;
inline constexpr std::uint32_t kRunQueueHint = 0x806A2D2Cu;
inline constexpr std::uint32_t kRunQueueBits = 0x806A2D30u;
inline constexpr std::uint32_t kRunQueue = 0x80650B90u;
inline constexpr std::uint32_t kIdleContext = 0x80650C90u;

// OSThread (the OSContext is its first 0x2C8 bytes).
inline constexpr std::uint32_t kThreadState = 0x2C8u;     // u16
inline constexpr std::uint32_t kThreadSuspend = 0x2CCu;   // s32
inline constexpr std::uint32_t kThreadPriority = 0x2D0u;  // s32 (effective)
inline constexpr std::uint32_t kThreadQueue = 0x2DCu;     // OSThreadQueue*
inline constexpr std::uint32_t kThreadLinkNext = 0x2E0u;
inline constexpr std::uint32_t kThreadLinkPrev = 0x2E4u;

// OSContext fields read when a thread first runs.
inline constexpr std::uint32_t kContextCr = 0x080u;
inline constexpr std::uint32_t kContextLr = 0x084u;
inline constexpr std::uint32_t kContextCtr = 0x088u;
inline constexpr std::uint32_t kContextXer = 0x08Cu;
inline constexpr std::uint32_t kContextFpr = 0x090u;
inline constexpr std::uint32_t kContextFpscr = 0x194u;
inline constexpr std::uint32_t kContextSrr0 = 0x198u;
inline constexpr std::uint32_t kContextSrr1 = 0x19Cu;
inline constexpr std::uint32_t kContextMode = 0x1A0u;     // u16
inline constexpr std::uint32_t kContextState = 0x1A2u;    // u16
inline constexpr std::uint32_t kContextGqr = 0x1A4u;
inline constexpr std::uint32_t kContextPsf = 0x1C8u;
inline constexpr std::uint16_t kContextStateFpSaved = 0x0001u;

inline constexpr std::uint16_t kStateReady = 1u;
inline constexpr std::uint16_t kStateRunning = 2u;
inline constexpr std::uint16_t kStateWaiting = 4u;
inline constexpr std::uint16_t kStateMoribund = 8u;

inline constexpr std::uint32_t kMsrFloatingPoint = 0x00002000u;
}  // namespace os

// The per-thread part of PpcContext (what an OSContext holds). SPRs, segment
// registers, HID2 and the timer fields are machine state and stay in place.
struct ThreadRegisters {
    std::uint32_t gpr[32];
    std::uint64_t fpr_bits[32];
    std::uint64_t ps1_bits[32];
    std::uint32_t cr;
    std::uint32_t lr;
    std::uint32_t ctr;
    std::uint32_t xer;
    std::uint32_t fpscr;
    std::uint32_t gqr[8];
    std::uint32_t msr;
    std::uint32_t pc;
    std::uint32_t reserved_address;
    std::uint32_t reserved_value;
};

void save_thread_registers(const PpcContext& context, ThreadRegisters& out) noexcept;
void load_thread_registers(const ThreadRegisters& in, PpcContext& context) noexcept;

// Big-endian access to guest RAM through the flat view.
struct GuestRam {
    std::byte* base = nullptr;

    [[nodiscard]] std::uint32_t load32(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint16_t load16(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint64_t load64(std::uint32_t address) const noexcept;
    void store32(std::uint32_t address, std::uint32_t value) const noexcept;
    void store16(std::uint32_t address, std::uint16_t value) const noexcept;
};

// Host state owned by one native call chain (the runtime's live scope depths).
// It is parked with a fiber when the fiber switches away and installed again
// when it resumes; a new fiber starts from the empty chain.
struct ChainState {
    std::uint64_t words[4]{};
};

class Scheduler {
public:
    struct Host {
        void* user = nullptr;
        // Runs a thread that has never executed on the calling (new) fiber:
        // loads its OSContext image into `context` and executes from SRR0 until
        // the thread exits. Returning means the exit path selected another
        // thread and this fiber was resumed with nothing left to run (fatal).
        void (*run_new_thread)(void* user, std::uint32_t thread, PpcContext* context) = nullptr;
        // Called with no runnable thread: blocks until an event may have made
        // one runnable and delivers due events. `context` is the register file
        // of the fiber that is idling; deliveries must leave it unchanged.
        void (*idle)(void* user, PpcContext* context) = nullptr;
        // Runs a guest function to completion on `context` (r3.. preset).
        void (*call_guest)(void* user, std::uint32_t function, PpcContext* context) = nullptr;
        // Parks the running chain's state in `parked` and installs `resumed`.
        void (*exchange_chain)(void* user, ChainState& parked, const ChainState& resumed) = nullptr;
    };

    Scheduler(Host host, GuestRam ram, PpcContext* shared_context);
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    ~Scheduler();

    // A guest thread's fiber.
    struct Fiber {
        int slot = -1;  // its stack in stacks_; -1 until it first runs
        ThreadRegisters saved{};
        ChainState chain{};
        bool started = false;
        bool boot = false;
    };

    // Runs `entry(argument)`, the guest's boot thread, on the boot fiber and returns or rethrows how it ended.
    // Call once, on the simulation thread.
    void run_boot(void (*entry)(void*), void* argument);

    // Rollback: every fiber's stack and the scheduler's own state. The rest of the machine (guest RAM, devices,
    // the runtime) is the caller's to save alongside.
    struct Snapshot {
        struct StackImage {
            int slot = -1;
            void* sp = nullptr;
            std::vector<std::byte> bytes;  // [sp, top)
        };
        std::vector<StackImage> stacks;
        std::unordered_map<std::uint32_t, Fiber> fibers;
        int current_slot = -1;
        std::uint32_t running_thread = 0u;
        std::uint32_t boot_thread = 0u;
        ChainState unowned_chain{};
        std::vector<int> retired;
        std::uint64_t switches = 0u, fibers_created = 0u, idle_entries = 0u, idle_passes = 0u;
        bool valid = false;
    };
    // Called on a fiber: parks it, captures every fiber into `out` from the simulation thread's own context and
    // resumes it. Returns false then; returns true when it is resumed instead by restore(out) later.
    bool save(Snapshot& out);
    // Called on a fiber: puts every fiber back as `in` captured it and resumes the fiber that called save(in),
    // inside that call (which returns true). This call does not return.
    [[noreturn]] void restore(const Snapshot& in);

    // SDK SelectThread(yield) on the shared register file. Returns with r3 = 0,
    // the SDK's value whether or not this thread was switched away and back.
    void select_thread(bool yield);

    // OSCreateThread finished for `thread`: a fiber left over from an earlier
    // thread at the same address (exited, parked forever) is released.
    void on_thread_created(std::uint32_t thread);

    [[nodiscard]] std::uint64_t switches() const noexcept { return switches_; }
    [[nodiscard]] std::uint64_t fibers_created() const noexcept { return fibers_created_; }
    [[nodiscard]] std::uint64_t idle_entries() const noexcept { return idle_entries_; }
    [[nodiscard]] std::uint64_t idle_passes() const noexcept { return idle_passes_; }
    // Guest thread whose fiber is executing (0 before the first selection).
    [[nodiscard]] std::uint32_t running_thread() const noexcept { return running_thread_; }

private:
    // kNoSlot: the simulation thread's own context (before run_boot, and after the boot fiber ends).
    static constexpr int kNoSlot = -1;

    enum class HostRequest : std::uint8_t { None, Save, Restore };

    struct Stack {
        std::byte* base = nullptr;  // allocation base (a no-access guard page, then the stack)
        std::byte* top = nullptr;   // one past the highest byte
        void* sp = nullptr;         // the parked context's stack pointer
        bool in_use = false;
    };

    static void fiber_entry(void* parameter);
    static void boot_entry(void* parameter);
    int acquire_stack(std::size_t size);
    void switch_context(int to_slot);
    void switch_to(std::uint32_t next);
    [[noreturn]] void return_to_boot() noexcept;
    ChainState& running_chain() noexcept;
    void set_current(std::uint32_t thread, std::uint32_t context_address) noexcept;
    void call_switch_callback(std::uint32_t old_thread, std::uint32_t new_thread);
    void enqueue_run(std::uint32_t thread) noexcept;
    void reap_exited();
    void release_parked();

    Host host_;
    GuestRam ram_;
    PpcContext* context_;
    std::unordered_map<std::uint32_t, Fiber> fibers_;
    std::vector<Stack> stacks_;
    void* host_sp_ = nullptr;      // the simulation thread's context while a fiber runs
    int current_slot_ = kNoSlot;   // the context executing now
    int boot_slot_ = kNoSlot;
    void (*boot_function_)(void*) = nullptr;
    void* boot_argument_ = nullptr;
    std::exception_ptr boot_result_{};
    // A fiber's request for the simulation thread's context (save / restore), and its operands.
    HostRequest host_request_ = HostRequest::None;
    int requester_slot_ = kNoSlot;
    Snapshot* save_target_ = nullptr;
    const Snapshot* restore_source_ = nullptr;
    bool resumed_by_restore_ = false;
    void host_save(Snapshot& out);
    int host_restore(const Snapshot& in);
    std::uint32_t boot_thread_ = 0u;
    // Chain of a fiber no thread owns yet (the boot fiber before its first
    // SelectThread).
    ChainState unowned_chain_{};
    std::uint32_t running_thread_ = 0u;
    unsigned int mxcsr_ = 0u;
    // Stacks of exited or recreated threads' fibers; released from another fiber.
    std::vector<int> retired_;
    std::exception_ptr fiber_error_{};
    std::uint64_t switches_ = 0u;
    std::uint64_t fibers_created_ = 0u;
    std::uint64_t idle_entries_ = 0u;
    std::uint64_t idle_passes_ = 0u;
};

}  // namespace galaxy::coop
