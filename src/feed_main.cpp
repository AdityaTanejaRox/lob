// Standalone demo producer: creates /tzu_ring in shared memory and streams the synthetic feed into it.
// Then run:  ./tzu_dash --shm /tzu_ring      (this is exactly how your colo app will talk to the UI)
//
// This is the executable that stands in for your real colo decoder process, so it wires up the
// "production" story end to end: async logging (logger.hpp), SIGINT/SIGTERM handling, an optional
// CPU-pin + real-time + mlockall hardening pass (harden.hpp, Linux only - opt in with --pin once you
// have the privileges for it), and a watchdog that requests shutdown if the feed loop ever stalls.
#include "tzu/demo_feed.hpp"
#include "tzu/harden.hpp"
#include "tzu/logger.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    std::string name = "/tzu_ring";
    int pinCore = -1, rtPriority = 10;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pin") && i + 1 < argc) pinCore = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--rt") && i + 1 < argc) rtPriority = std::atoi(argv[++i]);
        else name = argv[i];
    }

    tzu::Logger::instance().start();
    tzu::installSignalHandlers();

    std::unique_ptr<tzu::ProductionGuard> guard;
    if (pinCore >= 0) {
        guard = std::make_unique<tzu::ProductionGuard>(tzu::ProductionGuard::activate(pinCore, rtPriority));
        TZU_LOG_INFO("feed", "hardening: pinned=%d realtime=%d mem_locked=%d (core=%d rt_priority=%d)",
                     int(guard->pinned()), int(guard->realtime()), int(guard->memLocked()), pinCore, rtPriority);
    }

    tzu::Ring ring = tzu::Ring::createShm(name, 1u << 20);
    tzu::Producer prod(ring);
    tzu::DemoFeed feed(prod);
    TZU_LOG_INFO("feed", "demo feed -> shm %s (Ctrl-C to stop)", name.c_str());

    tzu::Watchdog wd("feed", std::chrono::seconds(2), [] {
        TZU_LOG_ERROR("feed", "feed loop stalled (no heartbeat for 2s); requesting shutdown");
        tzu::requestShutdown();
    });
    feed.run(tzu::shutdownFlag(), [&] { wd.tick(); });

    TZU_LOG_INFO("feed", "shutting down (dropped=%llu)", (unsigned long long)ring.dropped());
    tzu::Logger::instance().stop();
    return 0;
}
