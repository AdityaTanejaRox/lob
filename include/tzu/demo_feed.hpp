// Synthetic ES future + ES 0DTE/weekly options feed, so you can see the dashboard move
// before wiring in the real decoder. Speaks exactly the Producer API your decoder will use.
#pragma once
#include "engine.hpp"
#include "gamma.hpp"
#include "producer.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <random>
#include <thread>
#include <vector>

namespace tzu {

class DemoFeed {
public:
    static constexpr int32_t kFut = 1;
    explicit DemoFeed(Producer& p, double f0 = 7756.5) : p_(p), F_(f0), rng_(42) {}

    void defs(uint64_t nowNs) {
        const int64_t tick = d2px(0.25), otick = d2px(0.05);
        p_.future(kFut, tick);
        const int64_t now = int64_t(nowNs / 1'000'000'000ull);
        const int64_t exps[2] = {now + 6 * 3600, now + 3 * 86400 + 6 * 3600};
        int32_t id = 1000;
        for (int e = 0; e < 2; ++e)
            for (int k = -30; k <= 30; ++k) {
                const double K = std::round(F_ / 5.0) * 5.0 + 5.0 * k;
                const double dist = (K - F_) / F_;
                for (int c = 0; c < 2; ++c) {
                    const bool call = c == 0;
                    // calls stacked above spot, puts below -> positive gamma wall above, short gamma below
                    const double base = (call ? (dist > -0.002 ? 1 : 0.15) : (dist < 0.002 ? 1 : 0.15)) * std::exp(-std::fabs(dist) * 90) * (e == 0 ? 6000 : 3500);
                    const int32_t oi = int32_t(base * (0.6 + 0.8 * u01()));
                    p_.option(id, kFut, call, d2px(K), exps[e], otick, oi);
                    opts_.push_back({id, K, call, double(exps[e])});
                    ++id;
                }
            }
        for (int sd = 0; sd < 2; ++sd) for (int l = 1; l <= 10; ++l) p_.book(nowNs, kFut, sd, 5, l, d2px(price(sd, l)), qty(), 3);
        refreshOptions(nowNs);
    }

    void step(uint64_t nowNs) {
        if (u01() < 0.35) { F_ += (u01() < 0.5 ? -0.25 : 0.25); drift_ = 0.97 * drift_ + (F_ > lastF_ ? 1 : -1) * 0.03; lastF_ = F_;
            for (int sd = 0; sd < 2; ++sd) for (int l = 1; l <= 10; ++l) p_.book(nowNs, kFut, sd, 5, l, d2px(price(sd, l)), qty(), 1 + int(u01() * 8)); }
        for (int i = 0; i < 3; ++i) {                               // random size churn
            const int sd = u01() < 0.5, l = 1 + int(u01() * 10);
            p_.book(nowNs, kFut, sd, 1, l, d2px(price(sd, l)), qty(), 1 + int(u01() * 8));
        }
        if (u01() < 0.25) {                                         // future trade
            const bool buy = u01() < 0.5; const uint32_t g = p_.newGroup();
            p_.trade(nowNs, kFut, d2px(price(buy ? 1 : 0, 1)), 1 + int(-std::log(1 - u01()) * 6), 1, buy ? 1 : 2, g);
        }
        if (u01() < 0.01) {                                         // option sweep / print
            const auto& o = opts_[size_t(u01() * opts_.size())];
            if (std::fabs(o.K - F_) < 40 && o.exp - nowNs * 1e-9 < 86400) {
                const double theo = B76::price(o.call, F_, o.K, std::max(60.0, o.exp - nowNs * 1e-9) / (365.25 * 86400), 0.15, 0.04);
                const uint32_t g = p_.newGroup(); const bool buy = u01() < 0.75; const int legs = u01() < 0.4 ? 3 : 1;
                for (int i = 0; i < legs; ++i)
                    p_.trade(nowNs, o.id, d2px(std::round((theo + (buy ? 0.05 : -0.05) * i) / 0.05) * 0.05 + 0.05), 40 + int(u01() * 400), 2, buy ? 1 : 2, g);
            }
        }
        if (nowNs - lastOpt_ > 500'000'000ull) refreshOptions(nowNs);
    }

    void run(std::atomic<bool>& stop, const std::function<void()>& heartbeat = {}) {
        defs(wallNs());
        while (!stop.load(std::memory_order_relaxed)) {
            step(wallNs());
            if (heartbeat) heartbeat();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

private:
    struct Opt { int32_t id; double K; bool call; double exp; };
    double u01() { return std::uniform_real_distribution<double>(0, 1)(rng_); }
    int32_t qty() { return 5 + int(u01() * u01() * 400); }
    double price(int sd, int l) const { return sd == 0 ? F_ - 0.125 - 0.25 * (l - 1) : F_ + 0.125 + 0.25 * (l - 1); }
    void refreshOptions(uint64_t nowNs) {
        lastOpt_ = nowNs;
        for (auto& o : opts_) {
            if (std::fabs(o.K - F_) > 0.05 * F_) continue;
            const double T = std::max(60.0, o.exp - nowNs * 1e-9) / (365.25 * 86400);
            const double iv = 0.15 + 0.4 * std::max(0.0, (F_ - o.K) / F_) + 0.1 * std::fabs(o.K - F_) / F_;
            const double th = B76::price(o.call, F_, o.K, T, iv, 0.04);
            const double bid = std::max(0.05, std::floor(th / 0.05) * 0.05 - 0.05), ask = bid + 0.10;
            for (int l = 1; l <= 3; ++l) {
                p_.book(nowNs, o.id, 0, 5, l, d2px(std::max(0.05, bid - 0.05 * (l - 1))), 10 + int(u01() * 50), 2);
                p_.book(nowNs, o.id, 1, 5, l, d2px(ask + 0.05 * (l - 1)), 10 + int(u01() * 50), 2);
            }
        }
    }
    Producer& p_; double F_, lastF_ = 0, drift_ = 0; std::mt19937_64 rng_; std::vector<Opt> opts_; uint64_t lastOpt_ = 0;
};

}  // namespace tzu
