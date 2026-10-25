# tzu_dash - realtime options-flow dashboard for CME MDP3 (C++20, OpenGL/ImGui)

## Build / run
    cmake -B build && cmake --build build -j
    ./build/tzu_tests                      # headless self-test (book, ring, Black-76, engine, sweep merge)
    ./build/tzu_dash                       # built-in synthetic feed
    ./build/tzu_feed /tzu_ring &           # OR: separate producer process...
    ./build/tzu_dash --shm /tzu_ring       # ...consumed through shared memory (this is the colo path)
Flags: `--primary <secId>` (future to chart), `--mult 50`, `--macro`, `--short-calls`, `--scale 1.25`.
No GL box? `cmake -B build -DTZU_BUILD_UI=OFF` builds tests + feed only.

## Plugging in your decoder (the only API you touch: include/tzu/producer.hpp)
    tzu::Ring ring = tzu::Ring::createShm("/tzu_ring", 1u << 20);   // in your colo app, once
    tzu::Producer px(ring);
    // in your 46/48/54/55/49 handlers - field mapping is documented at the top of producer.hpp
    px.book(transactTime, secId, side, mdUpdateAction, mdPriceLevel, mdEntryPxMantissa, size, nOrders);
    px.trade(transactTime, secId, pxMantissa, size, nOrders, aggressorSide, groupIdForThisMessage);
Prices are raw PRICE9 mantissas (exponent -9). Full ring = event dropped, never blocks.
Feed the ES future (channel 310) AND ES options (channel 311) through the same ring.

## Verify against your data
1. Dealer sign convention (`Config::dealerLongCalls`): calls = +gamma, puts = -gamma by default. An assumption, not truth.
2. MDP3 books are 10 levels deep, so the heatmap shows the top 10 levels only. For real depth feed MBO (msg 47) and extend Heatmap::sample.
3. Open interest comes from `DefP::oi` (OpenInterestQty in msg 55) or Stat 'C' entries.
4. Sweep = one 48 message hitting >=2 price levels. `group` must be constant per message.
5. Expiry hour default 20:00 UTC (15:00 CDT); use 21 in winter.

## Not built
Scorecard grid and the trade/paper-order panel from the screenshot, charm flow, replay controls.
