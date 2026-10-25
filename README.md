# tzu_dash - realtime options-flow dashboard for CME MDP3 (C++20, OpenGL/ImGui)

## Build / run
    cmake -B build && cmake --build build -j
    ./build/tzu_tests                      # headless self-test (book, ring, Black-76, engine, sweep merge, logger, harden, latency)
    ./build/tzu_dash                       # built-in synthetic feed
    ./build/tzu_feed /tzu_ring &           # OR: separate producer process...
    ./build/tzu_dash --shm /tzu_ring       # ...consumed through shared memory (this is the colo path)
Flags: `--primary <secId>` (future to chart), `--mult 50`, `--macro`, `--short-calls`, `--scale 1.25`.
`tzu_feed` (the colo-producer stand-in) additionally takes `--pin <core>` and `--rt <priority>`
(Linux only - see Production hardening below).
No GL box? `cmake -B build -DTZU_BUILD_UI=OFF` builds tests + feed only.

## Primary target: Linux servers
This is developed against MSVC/Windows for now (no WSL available yet in this environment), but the
intended deployment is a Linux server/colo box. Everything compiles and is tested on Windows too, but
the OS-level hardening in `harden.hpp` (CPU pinning, SCHED_FIFO, mlockall) is implemented for real only
under `#if defined(__linux__)`; on any other platform those calls are safe no-ops that return `false` so
the build never breaks, but they do nothing. Don't assume you're hardened anywhere but Linux.

## Plugging in your decoder (the only API you touch: include/tzu/producer.hpp)
    tzu::Ring ring = tzu::Ring::createShm("/tzu_ring", 1u << 20);   // in your colo app, once
    tzu::Producer px(ring);
    // in your 46/48/54/55/49 handlers - field mapping is documented at the top of producer.hpp
    px.book(transactTime, secId, side, mdUpdateAction, mdPriceLevel, mdEntryPxMantissa, size, nOrders);
    px.trade(transactTime, secId, pxMantissa, size, nOrders, aggressorSide, groupIdForThisMessage);
Prices are raw PRICE9 mantissas (exponent -9). Full ring = event dropped, never blocks.
Feed the ES future (channel 310) AND ES options (channel 311) through the same ring.

## Async logger (include/tzu/logger.hpp)
Allocation-free, off-hot-path logging: `TZU_LOG_INFO("tag", "fmt %d", x)` etc. format into a small
stack buffer (no heap, no std::string) and push one fixed-size POD record into a bounded
multi-producer/single-consumer queue (Vyukov's lock-free algorithm - multiple threads, e.g. the UI
thread and a feed thread, can log concurrently without blocking each other). A single background
thread drains the queue and does the actual `fwrite`. If the queue is ever full, the record is
dropped and counted (`Logger::instance().dropped()`) rather than blocking the caller.
Call `Logger::instance().start()` once at startup and `.stop()` at shutdown (flushes anything queued).
`setMinLevel(LogLevel::Warn)` filters below that level before any formatting happens (one atomic load).

## Latency budgets (include/tzu/latency.hpp)
`StageScope sc(Stage::EngineTick);` times a scope with `std::chrono::steady_clock` and records it
against a named pipeline stage (`ring_drain`, `engine_tick`, `gex_recompute`, `heat_sample`,
`frame_render`). `LatencyTracker::instance().setBudget(stage, maxAvgNs, maxNs)` + `.check()` flags a
stage that's exceeded its budget - `tzu_dash` logs a periodic (5s) avg/max/count report per stage via
the async logger. The default budgets in `main.cpp` are illustrative; tune them once you have a real
feed. This is cheap `steady_clock` timing, not rdtsc-cycle precision - fine for a UI dashboard, not a
colo hot path.

## Production hardening (include/tzu/harden.hpp)
`installSignalHandlers()` + `isShutdownRequested()`/`requestShutdown()` give every executable a clean
SIGINT/SIGTERM shutdown path (both `tzu_dash` and `tzu_feed` use this; Ctrl-C closes the GL window and
joins the feed thread cleanly instead of hard-killing the process). `tzu_feed --pin <core> --rt <prio>`
opts into `ProductionGuard`: pins that process to a CPU core, raises it to `SCHED_FIFO` priority, and
`mlockall`s its pages - Linux only, typically needs `CAP_SYS_NICE`/root, so check the logged
`pinned=/realtime=/mem_locked=` booleans rather than assuming it worked. `Watchdog` fires a callback
(feed_main.cpp requests shutdown) if the feed loop stops ticking it for 2s - use it on any thread whose
silent death you want to notice.

## Verify against your data
1. Dealer sign convention (`Config::dealerLongCalls`): calls = +gamma, puts = -gamma by default. An assumption, not truth.
2. MDP3 books are 10 levels deep, so the heatmap shows the top 10 levels only. For real depth feed MBO (msg 47) and extend Heatmap::sample.
3. Open interest comes from `DefP::oi` (OpenInterestQty in msg 55) or Stat 'C' entries.
4. Sweep = one 48 message hitting >=2 price levels. `group` must be constant per message.
5. Expiry hour default 20:00 UTC (15:00 CDT); use 21 in winter.

## Not built
Scorecard grid and the trade/paper-order panel from the screenshot, charm flow, replay controls.

