// Lightweight latency-budget instrumentation, modeled on Apex-Wire's aw-core::budget module:
// named pipeline stages, each timed with a cheap RAII scope and checked against an expected
// avg/max budget, so a regression shows up as a failed check instead of silently creeping in.
//
// Uses std::chrono::steady_clock (portable, backed by clock_gettime(CLOCK_MONOTONIC) on Linux,
// effectively nanosecond resolution) instead of rdtsc cycle-counting: tzu_dash is a UI dashboard
// reading an already-decoded event stream, not a colo hot path measured in single-digit cycles,
// so wall-clock nanoseconds are precise enough here and keep this header portable.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace tzu {

enum class Stage : int { RingDrain = 0, EngineTick, GexRecompute, HeatSample, FrameRender, Count };
inline const char* stageName(Stage s) {
    static const char* n[] = {"ring_drain", "engine_tick", "gex_recompute", "heat_sample", "frame_render"};
    return n[int(s)];
}

struct StageStats {
    uint64_t count = 0, totalNs = 0, maxNs = 0;
    void record(uint64_t ns) { ++count; totalNs += ns; if (ns > maxNs) maxNs = ns; }
    uint64_t avgNs() const { return count ? totalNs / count : 0; }
};

// Single-threaded by design (every stage above is only ever timed from the UI thread, same
// assumption Engine itself already makes - see engine.hpp), so plain counters are fine, no atomics.
class LatencyTracker {
public:
    static LatencyTracker& instance() { static LatencyTracker t; return t; }

    void record(Stage s, uint64_t ns) { stats_[size_t(s)].record(ns); }
    const StageStats& stats(Stage s) const { return stats_[size_t(s)]; }

    struct Budget { uint64_t maxAvgNs, maxNs; };
    void setBudget(Stage s, uint64_t maxAvgNs, uint64_t maxNs) {
        budgets_[size_t(s)] = {maxAvgNs, maxNs}; haveBudget_[size_t(s)] = true;
    }

    // Evaluates every stage with a configured budget; returns false if any of them failed.
    // If `out` is non-null, prints a one-line-per-stage report (mirrors Apex-Wire's BudgetReport::print).
    bool check(FILE* out = nullptr) const {
        bool allPass = true;
        if (out) std::fprintf(out, "%-14s %10s %10s %10s %6s\n", "stage", "avg(us)", "max(us)", "count", "pass");
        for (int i = 0; i < int(Stage::Count); ++i) {
            const StageStats& s = stats_[size_t(i)];
            if (!haveBudget_[size_t(i)] || s.count == 0) continue;
            const Budget& b = budgets_[size_t(i)];
            const bool pass = s.avgNs() <= b.maxAvgNs && s.maxNs <= b.maxNs;
            allPass = allPass && pass;
            if (out) std::fprintf(out, "%-14s %10.2f %10.2f %10llu %6s\n", stageName(Stage(i)),
                                   s.avgNs() / 1000.0, s.maxNs / 1000.0, (unsigned long long)s.count, pass ? "OK" : "FAIL");
        }
        return allPass;
    }

private:
    std::array<StageStats, size_t(Stage::Count)> stats_{};
    std::array<Budget, size_t(Stage::Count)> budgets_{};
    std::array<bool, size_t(Stage::Count)> haveBudget_{};
};

// RAII: times the scope and records it against `stage` on destruction.
class StageScope {
public:
    explicit StageScope(Stage s) : stage_(s), t0_(std::chrono::steady_clock::now()) {}
    ~StageScope() {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count();
        LatencyTracker::instance().record(stage_, uint64_t(ns));
    }
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;

private:
    Stage stage_;
    std::chrono::steady_clock::time_point t0_;
};

}  // namespace tzu
