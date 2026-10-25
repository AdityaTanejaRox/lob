// Async, allocation-free logger, ported from the 3-stage design in
// papannnn/Trading-Engine-Simulator's "Low Latency Async Logger" notes:
//   1. A hot-path thread formats its message into a small stack buffer (snprintf, no heap,
//      no std::string) and enqueues one fixed-size POD record. No syscall happens on that thread.
//   2. A single background thread drains the queue and does the actual fwrite/syscall.
//   3. Unlike the SPSC ring in ring.hpp (one producer: the market-data decoder), tzu_dash has
//      *multiple* threads that may want to log at once (UI thread + feed thread), so the queue
//      here is a bounded multi-producer / single-consumer queue (Dmitry Vyukov's public-domain
//      algorithm, http://www.1024cores.net), not plain SPSC.
//
// A third "format worker" stage (raw bytes captured on the hot thread, formatted into a string on
// a dedicated middle thread) was deliberately left out: vsnprintf into a ~190-byte stack buffer is
// tens of nanoseconds, and tzu_dash only logs on meaningful transitions (connect/disconnect, drops,
// periodic stats) rather than per-market-event, so that extra thread/complexity isn't worth it here.
// If that ever changes, LogRecord already carries pre-rendered bytes, so a formatter stage could be
// inserted later without touching any call site.
#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <thread>

namespace tzu {

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };
inline const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        default:              return "ERROR";
    }
}

namespace detail {

// Bounded multi-producer / single-consumer queue (Vyukov's algorithm). Capacity must be a
// power of two. Lock-free on the push side so multiple logging threads never block each other;
// the pop side is only ever called from the logger's own background thread.
template <class T, uint64_t Capacity>
class MpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    MpscQueue() { for (uint64_t i = 0; i < Capacity; ++i) cells_[i].seq.store(i, std::memory_order_relaxed); }

    bool tryPush(const T& v) {
        Cell* cell;
        uint64_t pos = enqPos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            const uint64_t seq = cell->seq.load(std::memory_order_acquire);
            const int64_t diff = int64_t(seq) - int64_t(pos);
            if (diff == 0) {
                if (enqPos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false;                                            // full
            } else {
                pos = enqPos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = v;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) {
        Cell* cell;
        uint64_t pos = deqPos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            const uint64_t seq = cell->seq.load(std::memory_order_acquire);
            const int64_t diff = int64_t(seq) - int64_t(pos + 1);
            if (diff == 0) {
                if (deqPos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false;                                            // empty
            } else {
                pos = deqPos_.load(std::memory_order_relaxed);
            }
        }
        out = cell->data;
        cell->seq.store(pos + kMask + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr uint64_t kMask = Capacity - 1;
    struct Cell { std::atomic<uint64_t> seq; T data; };
    Cell cells_[Capacity];
    alignas(64) std::atomic<uint64_t> enqPos_{0};
    alignas(64) std::atomic<uint64_t> deqPos_{0};
};

}  // namespace detail

struct LogRecord {
    uint64_t ts = 0;               // ns since epoch
    LogLevel level = LogLevel::Info;
    char tag[12] = {};             // short component name, e.g. "feed", "engine", "ui"
    char msg[196] = {};            // pre-rendered message, always null-terminated
};
static_assert(sizeof(LogRecord) <= 256, "keep LogRecord small: it is copied by value through the queue");

// Single process-wide async logger. Call start() once at startup and stop() at shutdown;
// stop() drains and prints anything still queued (flush-on-exit), then joins the worker.
class Logger {
public:
    static Logger& instance() { static Logger l; return l; }

    void start(FILE* sink = stdout) {
        if (running_.exchange(true, std::memory_order_acq_rel)) return;
        sink_ = sink;
        worker_ = std::thread([this] { run(); });
    }
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (worker_.joinable()) worker_.join();
        drain();
    }
    void setMinLevel(LogLevel lv) { minLevel_.store(lv, std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    // Formats into a stack buffer and enqueues a POD record. No heap allocation, no syscalls.
    void logf(LogLevel level, const char* tag, const char* fmt, ...) {
        if (level < minLevel_.load(std::memory_order_relaxed)) return;    // cheap: one atomic load
        LogRecord r; r.ts = nowNs(); r.level = level;
        std::snprintf(r.tag, sizeof r.tag, "%s", tag);
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(r.msg, sizeof r.msg, fmt, ap);
        va_end(ap);
        if (!queue_.tryPush(r)) dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Logger() { stop(); }

private:
    Logger() = default;

    static uint64_t nowNs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    void run() {
        LogRecord r;
        while (running_.load(std::memory_order_acquire)) {
            if (queue_.tryPop(r)) print(r);
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    void drain() { LogRecord r; while (queue_.tryPop(r)) print(r); }
    void print(const LogRecord& r) {
        const std::time_t sec = std::time_t(r.ts / 1'000'000'000ull);
        const unsigned ms = unsigned((r.ts / 1'000'000ull) % 1000);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &sec);
#else
        localtime_r(&sec, &tmv);
#endif
        std::fprintf(sink_, "%02d:%02d:%02d.%03u %s [%-8s] %s\n",
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, levelName(r.level), r.tag, r.msg);
        std::fflush(sink_);     // keep log lines visible promptly under pipes/redirection/journald
    }

    detail::MpscQueue<LogRecord, 4096> queue_;
    std::atomic<LogLevel> minLevel_{LogLevel::Debug};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> running_{false};
    std::thread worker_;
    FILE* sink_ = stdout;
};

}  // namespace tzu

#define TZU_LOG(level, tag, ...) ::tzu::Logger::instance().logf(level, tag, __VA_ARGS__)
#define TZU_LOG_DEBUG(tag, ...)  TZU_LOG(::tzu::LogLevel::Debug, tag, __VA_ARGS__)
#define TZU_LOG_INFO(tag, ...)   TZU_LOG(::tzu::LogLevel::Info,  tag, __VA_ARGS__)
#define TZU_LOG_WARN(tag, ...)   TZU_LOG(::tzu::LogLevel::Warn,  tag, __VA_ARGS__)
#define TZU_LOG_ERROR(tag, ...)  TZU_LOG(::tzu::LogLevel::Error, tag, __VA_ARGS__)
