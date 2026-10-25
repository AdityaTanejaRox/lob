// Production hardening, ported from Apex-Wire's aw-core::harden module: graceful SIGINT/SIGTERM
// shutdown, CPU affinity pinning, SCHED_FIFO real-time priority, mlockall, and a heartbeat watchdog.
//
// tzu_dash's primary deployment target is a Linux server (the colo producer process in particular),
// so the OS-level calls below are implemented for real on __linux__. On any other platform (this is
// currently developed/built on Windows pending WSL) pin/realtime/mlock are safe no-ops that return
// false - callers are expected to check the return value and log accordingly, never to assume they
// took effect. Signal handling and the watchdog are plain std:: / libc and work everywhere.
#pragma once
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <thread>

#if defined(__linux__)
  #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
  #endif
  #include <sched.h>
  #include <sys/mman.h>
#endif

namespace tzu {

inline std::atomic<bool>& shutdownFlag() { static std::atomic<bool> f{false}; return f; }
inline bool isShutdownRequested() { return shutdownFlag().load(std::memory_order_acquire); }
inline void requestShutdown() { shutdownFlag().store(true, std::memory_order_release); }
inline void resetShutdown() { shutdownFlag().store(false, std::memory_order_release); }

namespace detail { inline void shutdownSignalHandler(int) { requestShutdown(); } }

// Installs SIGINT/SIGTERM -> requestShutdown(). Ignores SIGPIPE on POSIX (a dead TCP/shm peer
// should not kill the process; the ring already handles a vanished consumer/producer gracefully).
inline void installSignalHandlers() {
    std::signal(SIGINT, detail::shutdownSignalHandler);
    std::signal(SIGTERM, detail::shutdownSignalHandler);
#if defined(__unix__) || defined(__APPLE__)
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

// Pins the calling thread to a CPU core. Linux only (sched_setaffinity); no-op elsewhere.
inline bool pinThread(int core) {
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)core; return false;
#endif
}

// Raises the calling thread to SCHED_FIFO real-time priority. Linux only; typically needs
// CAP_SYS_NICE or root, so this commonly fails in dev environments - that's expected, not a bug.
inline bool setRealtimePriority(int priority) {
#if defined(__linux__)
    sched_param sp{}; sp.sched_priority = priority;
    return sched_setscheduler(0, SCHED_FIFO, &sp) == 0;
#else
    (void)priority; return false;
#endif
}

// Locks all current+future pages into RAM so the hot path never takes a page fault. Linux only.
inline bool lockMemory() {
#if defined(__linux__)
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#else
    return false;
#endif
}
inline void unlockMemory() {
#if defined(__linux__)
    munlockall();
#endif
}

// Background liveness check: if tick() hasn't been called for `timeout` wall-clock time, fires
// onTimeout() once (from the watchdog's own thread) and stops. Typical use: the feed loop calls
// tick() once per iteration; onTimeout() logs an error and calls requestShutdown().
class Watchdog {
public:
    Watchdog(const char* /*name*/, std::chrono::milliseconds timeout, std::function<void()> onTimeout)
        : timeout_(timeout), onTimeout_(std::move(onTimeout)), thread_([this] { run(); }) {}
    ~Watchdog() { stop(); }

    void tick() { heartbeat_.fetch_add(1, std::memory_order_release); }
    void stop() {
        if (running_.exchange(false, std::memory_order_acq_rel) && thread_.joinable()) thread_.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    void run() {
        uint64_t lastTick = heartbeat_.load(std::memory_order_acquire);
        auto lastCheck = std::chrono::steady_clock::now();
        while (running_.load(std::memory_order_acquire) && !isShutdownRequested()) {
            std::this_thread::sleep_for(timeout_ / 2);
            const uint64_t cur = heartbeat_.load(std::memory_order_acquire);
            const auto now = std::chrono::steady_clock::now();
            if (cur != lastTick) { lastTick = cur; lastCheck = now; }
            else if (now - lastCheck > timeout_) { onTimeout_(); break; }
        }
    }
    std::chrono::milliseconds timeout_;
    std::function<void()> onTimeout_;
    std::atomic<bool> running_{true};
    std::atomic<uint64_t> heartbeat_{0};
    std::thread thread_;
};

// RAII bundle mirroring Apex-Wire's ProductionGuard: pins the calling thread, raises its
// scheduling priority, and locks memory. Unlocks memory on destruction if it was locked.
// Construct it on the thread you actually want hardened (e.g. the feed/decoder thread), not on
// the UI thread - a GL/ImGui render loop has no business running SCHED_FIFO or mlockall'd.
class ProductionGuard {
public:
    static ProductionGuard activate(int core, int rtPriority) {
        ProductionGuard g;
        g.pinned_ = pinThread(core);
        g.realtime_ = setRealtimePriority(rtPriority);
        g.memLocked_ = lockMemory();
        return g;
    }
    bool pinned() const { return pinned_; }
    bool realtime() const { return realtime_; }
    bool memLocked() const { return memLocked_; }

    ProductionGuard(ProductionGuard&& o) noexcept { *this = std::move(o); }
    ProductionGuard& operator=(ProductionGuard&& o) noexcept {
        if (memLocked_) unlockMemory();
        pinned_ = o.pinned_; realtime_ = o.realtime_; memLocked_ = o.memLocked_;
        o.pinned_ = o.realtime_ = o.memLocked_ = false;
        return *this;
    }
    ProductionGuard(const ProductionGuard&) = delete;
    ProductionGuard& operator=(const ProductionGuard&) = delete;
    ~ProductionGuard() { if (memLocked_) unlockMemory(); }

private:
    ProductionGuard() = default;
    bool pinned_ = false, realtime_ = false, memLocked_ = false;
};

}  // namespace tzu
