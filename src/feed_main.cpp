// Standalone demo producer: creates /tzu_ring in shared memory and streams the synthetic feed into it.
// Then run:  ./tzu_dash --shm /tzu_ring      (this is exactly how your colo app will talk to the UI)
#include "tzu/demo_feed.hpp"
#include <csignal>
#include <cstdio>

static std::atomic<bool> g_stop{false};
int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "/tzu_ring";
    std::signal(SIGINT, [](int) { g_stop = true; });
    tzu::Ring ring = tzu::Ring::createShm(name, 1u << 20);
    tzu::Producer prod(ring);
    tzu::DemoFeed feed(prod);
    std::printf("demo feed -> shm %s (Ctrl-C to stop)\n", name.c_str());
    feed.run(g_stop);
    return 0;
}
