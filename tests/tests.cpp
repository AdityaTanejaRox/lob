#include "tzu/demo_feed.hpp"
#include "tzu/engine.hpp"
#include "tzu/harden.hpp"
#include "tzu/latency.hpp"
#include "tzu/logger.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>
using namespace tzu;

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static Level L(double px, int q) { return Level{d2px(px), q, 1}; }

int main() {
    // ---- book semantics ----
    Book b;
    b.apply(0, 0, 1, L(100, 5)); b.apply(0, 0, 2, L(99.75, 7)); b.apply(0, 0, 1, L(100.25, 3));   // insert at top shifts down
    CHECK(b.n[0] == 3 && b.s[0][0].px == d2px(100.25) && b.s[0][1].px == d2px(100) && b.s[0][2].px == d2px(99.75));
    b.apply(0, 2, 2, {});                                                                         // delete level 2 shifts up
    CHECK(b.n[0] == 2 && b.s[0][1].px == d2px(99.75));
    b.apply(0, 1, 1, L(100.25, 9)); CHECK(b.s[0][0].qty == 9);
    b.apply(0, 4, 1, {});                                                                         // DeleteFrom 1..1
    CHECK(b.n[0] == 1 && b.s[0][0].px == d2px(99.75));
    b.apply(0, 3, 1, {}); CHECK(b.n[0] == 0);
    for (int i = 1; i <= 10; ++i) b.apply(1, 0, i, L(101 + i, i));
    b.apply(1, 0, 1, L(100.5, 1)); CHECK(b.n[1] == 10 && b.s[1][0].px == d2px(100.5) && b.s[1][9].px == d2px(110)); // overflow trims tail

    // ---- ring (threaded SPSC) ----
    Ring ring = Ring::heap(1 << 12); Producer pr(ring);
    std::thread t([&] { for (int i = 0; i < 100000; ++i) { while (ring.backlog() > 4000) {} pr.stat(i, StOpenInterest, i); } });
    uint64_t seen = 0; Event e; int32_t nextId = 0; bool ordered = true;
    while (seen < 100000) if (ring.tryPop(e)) { ordered &= (e.secId == nextId++); ++seen; }
    t.join(); CHECK(ordered && ring.dropped() == 0);

    // ---- black-76 ----
    const double F = 7750, K = 7800, T = 3.0 / 365.25, v = 0.17, r = 0.04;
    CHECK(std::fabs(B76::iv(true, B76::price(true, F, K, T, v, r), F, K, T, r) - v) < 1e-6);
    CHECK(std::fabs(B76::iv(false, B76::price(false, F, K, T, v, r), F, K, T, r) - v) < 1e-6);
    double g = B76::gamma(F, K, T, v, r), h = 0.01;   // gamma = d2P/dF2
    double fd = (B76::price(true, F + h, K, T, v, r) - 2 * B76::price(true, F, K, T, v, r) + B76::price(true, F - h, K, T, v, r)) / (h * h);
    CHECK(std::fabs(g - fd) / g < 1e-3);

    // ---- full pipeline on the demo feed ----
    Ring r2 = Ring::heap(1 << 18); Producer p2(r2); DemoFeed feed(p2);
    Engine eng; uint64_t t0 = wallNs();
    feed.defs(t0);
    for (int i = 0; i < 3000; ++i) { feed.step(t0 + uint64_t(i) * 2'000'000ull); }
    r2.drain([&](const Event& ev) { eng.process(ev); }, 1 << 20);
    eng.tick(t0 + 1); eng.tick(t0 + 6'100'000'000ull);
    std::printf("events=%llu dropped=%llu spot=%.2f instr=%zu tape=%zu bubbles=%zu\n", (unsigned long long)eng.eventsSeen, (unsigned long long)r2.dropped(), eng.spot(), eng.instr.size(), eng.tape.size(), eng.bubbles.size());
    CHECK(eng.gex.valid);
    std::printf("gex: strikes=%zu net=%.3eB flip=%.2f callWall=%.0f accel=%.0f EM=%.1f atmIV=%.3f drift=%.2eM\n", eng.gex.rows.size(), eng.gex.netGex / 1e9, eng.gex.flip, eng.gex.callWall, eng.gex.accel, eng.gex.em, eng.gex.atmIv, eng.drift / 1e6);
    CHECK(eng.gex.callWall > eng.spot() && eng.gex.em > 3 && eng.gex.em < 60);
    CHECK(!eng.tape.empty());
    bool anySweep = false; for (auto& tr : eng.tape) anySweep |= tr.sweep;
    std::printf("tape[0]: strike=%.0f %c qty=%d sweep=%d grade=%c score=%d | anySweep=%d\n", eng.tape[0].strike, eng.tape[0].call ? 'C' : 'P', eng.tape[0].qty, eng.tape[0].sweep, eng.tape[0].grade, eng.tape[0].score, anySweep);
    bool fd2; auto pend = eng.heat.takePending(fd2);
    std::printf("heatmap: head=%d pending=%zu fullDirty=%d\n", eng.heat.head(), pend.size(), fd2);
    CHECK(eng.heat.head() > 0 || !pend.empty());
    // ---- sweep merge: 3 legs, same group, rising price -> one tape row ----
    {
        int32_t opt = 0; for (auto& [id, in] : eng.instr) if (in.d.kind == KindCall) { opt = id; break; }
        Producer pp(r2); uint32_t grp = pp.newGroup() + 9000; const size_t before = eng.tape.size();
        for (int i = 0; i < 3; ++i) pp.trade(t0, opt, d2px(2.0 + 0.05 * i), 100, 2, 1, grp);
        r2.drain([&](const Event& ev) { eng.process(ev); }, 100);
        CHECK(eng.tape.size() == before + 1 && eng.tape[0].sweep && eng.tape[0].nPx == 3 && eng.tape[0].qty == 300);
    }

    // ---- async logger: MPSC queue correctness under concurrent producers ----
    {
        using Q = detail::MpscQueue<uint64_t, 1 << 14>;
        Q q; constexpr int kProducers = 4, kPerProducer = 20000;
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p)
            producers.emplace_back([&, p] { for (int i = 0; i < kPerProducer; ++i) { uint64_t v = uint64_t(p) * 1'000'000ull + uint64_t(i); while (!q.tryPush(v)) {} } });
        std::unordered_set<uint64_t> seen; uint64_t val; size_t total = kProducers * kPerProducer;
        while (seen.size() < total) if (q.tryPop(val)) seen.insert(val);
        for (auto& t : producers) t.join();
        CHECK(seen.size() == total);                                      // no duplicates, nothing lost
    }

    // ---- async logger: format + level filter + flush-on-stop round trip ----
    {
        const char* path = "tzu_logger_test.tmp";
        FILE* f = std::fopen(path, "w+");
        CHECK(f != nullptr);
        Logger::instance().start(f);
        Logger::instance().setMinLevel(LogLevel::Warn);
        TZU_LOG_DEBUG("test", "should not appear %d", 1);                 // filtered out below Warn
        TZU_LOG_WARN("test", "hello %d", 42);
        Logger::instance().stop();                                        // joins worker + flushes remaining queue
        std::fflush(f); std::rewind(f);
        char buf[4096] = {}; const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        std::fclose(f); std::remove(path);
        CHECK(std::strstr(buf, "should not appear") == nullptr);
        CHECK(std::strstr(buf, "hello 42") != nullptr);
        Logger::instance().setMinLevel(LogLevel::Debug);                  // restore default for any later use
    }

    // ---- harden: shutdown flag round trip + watchdog fires only on real stalls ----
    {
        resetShutdown(); CHECK(!isShutdownRequested());
        requestShutdown(); CHECK(isShutdownRequested());
        resetShutdown(); CHECK(!isShutdownRequested());
#if !defined(__linux__)
        CHECK(pinThread(0) == false); CHECK(setRealtimePriority(1) == false); CHECK(lockMemory() == false);
#endif
        std::atomic<bool> fired{false};
        {
            Watchdog wd("test", std::chrono::milliseconds(300), [&] { fired = true; });
            for (int i = 0; i < 8; ++i) { wd.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
            CHECK(!fired.load());
        }
        {
            Watchdog wd("test2", std::chrono::milliseconds(150), [&] { fired = true; });
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            CHECK(fired.load());
        }
        resetShutdown();                                                  // the fired watchdog above didn't call this; keep state clean regardless
    }

    // ---- latency budgets: StageScope records real elapsed time; check() flags an impossible budget ----
    {
        auto& lt = LatencyTracker::instance();
        { StageScope sc(Stage::RingDrain); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(lt.stats(Stage::RingDrain).count >= 1 && lt.stats(Stage::RingDrain).avgNs() > 0);
        lt.setBudget(Stage::RingDrain, 1, 1);
        CHECK(!lt.check());
        lt.setBudget(Stage::RingDrain, 1'000'000'000ull, 1'000'000'000ull);
        CHECK(lt.check());
    }

    std::puts("ALL OK"); return 0;
}
