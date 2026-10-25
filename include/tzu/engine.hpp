// Consumes Events, owns all derived state the UI draws. Single-threaded: UI thread drains the
// ring, calls process() per event and tick() once per frame. No locks.
#pragma once
#include "book.hpp"
#include "events.hpp"
#include "gamma.hpp"
#include "heatmap.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

namespace tzu {

inline uint64_t wallNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

struct Config {
    int32_t  primaryFuture = 0;        // SecurityID of the future to chart (0 = first future definition seen)
    double   multiplier    = 50.0;     // $/point (ES = 50)
    double   rate          = 0.04;
    bool     dealerLongCalls = true;   // true: call OI = dealer long gamma (+), put OI = dealer short gamma (-). Flip if you disagree.
    bool     macroDay      = false;    // flips the "Macro day" grade box
    int      gradeA = 5, gradeB = 3;   // boxes ticked (of 7) needed for A / B
    uint64_t heatDtNs = 100'000'000;   // one heatmap column every 100 ms
    double   strikeBand = 0.08;        // only strikes within +-8% of spot enter the gamma profile
};

struct Instr { DefP d{}; int32_t oi = 0; int64_t cumVol = 0; };
struct Bubble { uint64_t ts; double px; int qty; uint8_t aggr; };

enum GradeBit { G_Repeat, G_NewMoney, G_PaidUp, G_WithDay, G_DealersShort, G_ClearPath, G_Macro, G_COUNT };
inline const char* gradeLabel(int b) {
    static const char* n[] = {"Repeat buyer", "New money", "Paid up", "With the day", "Dealers short gamma", "Clear path", "Macro day"};
    return n[b];
}

struct TapeRow {
    uint64_t ts = 0; int32_t sec = 0; uint32_t group = 0;
    double strike = 0, px = 0; bool call = true; int qty = 0, nPx = 1; uint8_t aggr = 0;
    bool sweep = false; int score = 0; char grade = 'C'; uint8_t bits = 0;
    double lastPx = 0, firstPx = 0;
};

class Engine {
public:
    explicit Engine(Config c = {}) : cfg(c), heat(1024, 320) {}

    Config cfg;
    Heatmap heat;
    std::unordered_map<int32_t, Book> books;
    std::unordered_map<int32_t, Instr> instr;
    std::deque<Bubble> bubbles;
    std::deque<TapeRow> tape;                 // newest at front
    GexProfile gex;
    double dayRef = 0, dayHigh = 0, dayLow = 0, lastTrade = 0;
    double drift = 0, drift5m = 0;            // signed delta-notional of option prints ($)
    uint64_t eventsSeen = 0;

    double spot() const {
        auto it = books.find(cfg.primaryFuture);
        if (it != books.end() && it->second.hasBoth()) return it->second.mid();
        return lastTrade;
    }
    const Book* primaryBook() const { auto it = books.find(cfg.primaryFuture); return it == books.end() ? nullptr : &it->second; }

    void process(const Event& e) {
        ++eventsSeen;
        switch (e.type) {
            case EvType::Reset: books[e.secId].clear(); break;
            case EvType::Book:  books[e.secId].apply(e.b.side, e.b.action, e.b.level, Level{e.b.px, e.b.qty, e.b.orders}); break;
            case EvType::Def: {
                Instr& in = instr[e.secId]; in.d = e.d; in.oi = e.d.oi;
                if (e.d.kind == KindFuture && cfg.primaryFuture == 0) cfg.primaryFuture = e.secId;
                if (e.secId == cfg.primaryFuture) heat.setTick(e.d.tick);
                break;
            }
            case EvType::Stat: {
                Instr& in = instr[e.secId];
                if (e.s.kind == StOpenInterest) in.oi = int32_t(e.s.value);
                else if (e.s.kind == StClearedVol) in.cumVol = e.s.value;
                else if ((e.s.kind == StOpen || e.s.kind == StSettle) && e.secId == cfg.primaryFuture && dayRef == 0) dayRef = px2d(e.s.value);
                break;
            }
            case EvType::Trade: onTrade(e); break;
        }
    }

    void tick(uint64_t nowNs) {
        if (cfg.primaryFuture) {
            auto it = books.find(cfg.primaryFuture);
            if (it != books.end()) {
                if (lastSample_ == 0) lastSample_ = nowNs;
                int guard = 0;
                while (nowNs - lastSample_ >= cfg.heatDtNs && guard++ < 32) { heat.sample(it->second); lastSample_ += cfg.heatDtNs; }
                if (guard >= 32) lastSample_ = nowNs;
            }
        }
        const uint64_t horizon = uint64_t(heat.cols()) * cfg.heatDtNs;
        while (!bubbles.empty() && nowNs > bubbles.front().ts && nowNs - bubbles.front().ts > horizon) bubbles.pop_front();
        while (tape.size() > 300) tape.pop_back();
        while (!flow_.empty() && nowNs > flow_.front().first && nowNs - flow_.front().first > 300'000'000'000ull) { drift5m -= flow_.front().second; flow_.pop_front(); }
        if (nowNs - lastGex_ > 250'000'000ull) { recomputeGex(nowNs); lastGex_ = nowNs; }
    }

private:
    // ---------------- trades ----------------
    void onTrade(const Event& e) {
        const double px = px2d(e.t.px);
        auto it = instr.find(e.secId);
        if (e.secId == cfg.primaryFuture || (it != instr.end() && it->second.d.kind == KindFuture)) {
            if (e.secId != cfg.primaryFuture) return;
            lastTrade = px; if (dayRef == 0) dayRef = px;
            dayHigh = dayHigh == 0 ? px : std::max(dayHigh, px); dayLow = dayLow == 0 ? px : std::min(dayLow, px);
            bubbles.push_back({e.ts, px, e.t.qty, e.t.aggr});
            if (bubbles.size() > 20000) bubbles.pop_front();
            return;
        }
        if (it == instr.end() || it->second.d.kind == KindFuture) return;
        Instr& in = it->second; in.cumVol += e.t.qty;
        const bool call = in.d.kind == KindCall;

        if (!tape.empty() && tape.front().group == e.t.group && tape.front().sec == e.secId && e.t.group != 0) {   // same sweep
            TapeRow& r = tape.front();
            r.px = (r.px * r.qty + px * e.t.qty) / double(r.qty + e.t.qty);
            r.qty += e.t.qty; if (px != r.lastPx) { ++r.nPx; r.lastPx = px; }
            r.sweep = r.nPx >= 2; grade(r, in);
            return;
        }
        TapeRow r; r.ts = e.ts; r.sec = e.secId; r.group = e.t.group; r.strike = px2d(in.d.strike); r.call = call;
        r.px = r.firstPx = r.lastPx = px; r.qty = e.t.qty; r.aggr = e.t.aggr;
        flowAdd(e.ts, in, e.t.aggr, e.t.qty, call);
        grade(r, in);
        auto& last = lastDir_[e.secId];                       // for "repeat buyer"
        r.bits |= (last[e.t.aggr & 3] && e.ts - last[e.t.aggr & 3] < 600'000'000'000ull) ? (1u << G_Repeat) : 0;
        last[e.t.aggr & 3] = e.ts;
        finishGrade(r);
        tape.push_front(r);
    }

    void grade(TapeRow& r, const Instr& in) {
        const double F = spot(); const uint8_t keepRepeat = r.bits & (1u << G_Repeat); r.bits = keepRepeat;
        const bool bullish = (r.call && r.aggr == 1) || (!r.call && r.aggr == 2);
        auto bk = books.find(r.sec);
        if (bk != books.end()) {
            if (r.aggr == 1 && bk->second.n[1] && r.px >= bk->second.ask() - 1e-9) r.bits |= 1u << G_PaidUp;
            if (r.aggr == 2 && bk->second.n[0] && r.px <= bk->second.bid() + 1e-9) r.bits |= 1u << G_PaidUp;
        }
        if (in.cumVol > in.oi) r.bits |= 1u << G_NewMoney;
        if (F > 0 && dayRef > 0 && r.aggr && ((F > dayRef) == bullish)) r.bits |= 1u << G_WithDay;
        for (const auto& g : gex.rows) if (g.strike == r.strike) { if (g.net < 0) r.bits |= 1u << G_DealersShort; break; }
        if (F > 0 && gex.valid) {
            bool clear = true; const double lo = std::min(F, r.strike), hi = std::max(F, r.strike);
            for (const auto& g : gex.rows) if (g.strike > lo && g.strike < hi && g.net > 0.5 * gex.maxAbs) { clear = false; break; }
            if (clear) r.bits |= 1u << G_ClearPath;
        }
        if (cfg.macroDay) r.bits |= 1u << G_Macro;
        finishGrade(r);
    }
    void finishGrade(TapeRow& r) {
        r.score = std::popcount(unsigned(r.bits));
        r.grade = r.score >= cfg.gradeA ? 'A' : r.score >= cfg.gradeB ? 'B' : 'C';
    }

    void flowAdd(uint64_t ts, const Instr& in, uint8_t aggr, int qty, bool call) {
        const double F = spot(); if (F <= 0 || aggr == 0) return;
        const double T = (double(in.d.expiryEpochS) - ts * 1e-9) / (365.25 * 86400); if (T <= 0) return;
        const double K = px2d(in.d.strike), iv = gex.atmIv > 0 ? gex.atmIv : 0.15;
        const double v = (aggr == 1 ? 1.0 : -1.0) * qty * B76::delta(call, F, K, T, iv, cfg.rate) * cfg.multiplier * F;
        drift += v; drift5m += v; flow_.emplace_back(ts, v);
    }

    // ---------------- dealer gamma profile ----------------
    void recomputeGex(uint64_t nowNs) {
        const double F = spot();
        if (F <= 0) { gex.valid = false; return; }
        struct Opt { double K, T, iv, oi; bool call; };
        std::vector<Opt> opts; opts.reserve(instr.size());
        const double nowS = nowNs * 1e-9; double atmDist = 1e18, atmIv = 0, atmT = 1.0 / 365.25;
        for (auto& [sec, in] : instr) {
            if (in.d.kind == KindFuture || in.oi <= 0) continue;
            const double K = px2d(in.d.strike); if (std::fabs(K - F) > cfg.strikeBand * F) continue;
            double T = (double(in.d.expiryEpochS) - nowS) / (365.25 * 86400); if (T <= 0) continue;
            T = std::max(T, 60.0 / (365.25 * 86400));
            double iv = std::nan("");
            auto b = books.find(sec);
            if (b != books.end() && b->second.hasBoth()) iv = B76::iv(in.d.kind == KindCall, b->second.mid(), F, K, T, cfg.rate);
            opts.push_back({K, T, iv, double(in.oi), in.d.kind == KindCall});
            if (!std::isnan(iv) && T < 5.0 / 365.25) {          // ATM = nearest expiry first, then nearest strike
                const double d = std::fabs(K - F);
                if (atmIv == 0 || T < atmT * 0.9 || (T < atmT * 1.1 && d < atmDist)) { atmDist = d; atmIv = iv; atmT = T; }
            }
        }
        if (atmIv <= 0) atmIv = 0.15;
        const double cs = cfg.dealerLongCalls ? 1.0 : -1.0;     // +1: calls add dealer long-gamma, puts subtract
        std::map<int64_t, GexRow> rows;
        for (auto& o : opts) {
            const double iv = std::isnan(o.iv) ? atmIv : o.iv;
            const double g = B76::gamma(F, o.K, o.T, iv, cfg.rate) * o.oi * cfg.multiplier * F * F * 0.01;
            GexRow& r = rows[int64_t(std::llround(o.K * 100))]; r.strike = o.K;
            if (o.call) { r.callGex += cs * g; r.callOI += o.oi; } else { r.putGex -= cs * g; r.putOI += o.oi; }
        }
        GexProfile p; p.spot = F; p.atmIv = atmIv; p.em = F * atmIv * std::sqrt(atmT);   // expected move to the nearest expiry (0DTE-style, like the +-18 tile)
        double bestUp = 0, bestDn = 0;
        for (auto& [k, r] : rows) {
            r.net = r.callGex + r.putGex; p.netGex += r.net; p.maxAbs = std::max(p.maxAbs, std::fabs(r.net));
            if (r.strike > F && r.net > bestUp) { bestUp = r.net; p.callWall = r.strike; }
            if (std::fabs(r.strike - F) < 0.015 * F && r.net < bestDn) { bestDn = r.net; p.accel = r.strike; }
            p.rows.push_back(r);
        }
        // gamma flip: where total dealer gamma changes sign as spot moves, nearest to spot
        auto total = [&](double S) {
            double t = 0;
            for (auto& o : opts) {
                const double iv = std::isnan(o.iv) ? atmIv : o.iv;
                const double g = B76::gamma(S, o.K, o.T, iv, cfg.rate) * o.oi * cfg.multiplier * S * S * 0.01;
                t += (o.call ? cs : -cs) * g;
            }
            return t;
        };
        if (!opts.empty()) {
            double prevS = F * 0.97, prev = total(prevS), best = 0;
            for (int i = 1; i <= 60; ++i) {
                const double S = F * (0.97 + 0.001 * i), v = total(S);
                if ((prev < 0) != (v < 0)) {
                    const double x = prevS + (S - prevS) * (0.0 - prev) / (v - prev);
                    if (best == 0 || std::fabs(x - F) < std::fabs(best - F)) best = x;
                }
                prev = v; prevS = S;
            }
            p.flip = best;
        }
        p.valid = !p.rows.empty(); gex = std::move(p);
    }

    uint64_t lastSample_ = 0, lastGex_ = 0;
    std::deque<std::pair<uint64_t, double>> flow_;
    std::unordered_map<int32_t, std::array<uint64_t, 4>> lastDir_;
};

}  // namespace tzu
