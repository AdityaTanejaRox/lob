// Mirror of a CME MDP3 market-by-price book (10 explicit levels), applying MDUpdateAction semantics.
#pragma once
#include <algorithm>
#include <cstdint>

namespace tzu {

struct Level { int64_t px = 0; int32_t qty = 0; int32_t orders = 0; };

struct Book {
    static constexpr int D = 10;
    Level s[2][D];          // [0]=bids best-first, [1]=asks best-first
    int n[2] = {0, 0};

    void clear() { n[0] = n[1] = 0; }
    bool hasBoth() const { return n[0] > 0 && n[1] > 0; }
    double bid() const { return n[0] ? s[0][0].px * 1e-9 : 0; }
    double ask() const { return n[1] ? s[1][0].px * 1e-9 : 0; }
    double mid() const { return hasBoth() ? 0.5 * (bid() + ask()) : 0; }

    // action: 0 New, 1 Change, 2 Delete, 3 DeleteThru, 4 DeleteFrom(1..level), 5 Overlay. level is 1-based.
    void apply(int side, int action, int level, const Level& lv) {
        if (side < 0 || side > 1) return;
        Level* a = s[side]; int& cnt = n[side]; int i = level - 1;
        switch (action) {
            case 0:
                if (i < 0 || i >= D) return;
                if (i > cnt) i = cnt;                         // tolerate a gap
                for (int j = std::min(cnt, D - 1); j > i; --j) a[j] = a[j - 1];
                a[i] = lv; cnt = std::min(cnt + 1, D); break;
            case 1: case 5:
                if (i < 0 || i >= D) return;
                a[i] = lv; if (i >= cnt) cnt = i + 1; break;
            case 2:
                if (i < 0 || i >= cnt) return;
                for (int j = i; j < cnt - 1; ++j) a[j] = a[j + 1];
                --cnt; break;
            case 3: cnt = 0; break;
            case 4: {
                int k = std::min(level, cnt);
                for (int j = 0; j < cnt - k; ++j) a[j] = a[j + k];
                cnt -= k; break;
            }
        }
    }
    int32_t maxQty() const {
        int32_t m = 1; for (int sd = 0; sd < 2; ++sd) for (int i = 0; i < n[sd]; ++i) m = std::max(m, s[sd][i].qty); return m;
    }
};

}  // namespace tzu
