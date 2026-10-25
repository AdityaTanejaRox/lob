// Bookmap-style liquidity heatmap. One column = one book sample; columns form a ring so the
// GPU texture only ever receives ONE new 1-pixel-wide column per sample (glTexSubImage2D).
#pragma once
#include "book.hpp"
#include <cmath>
#include <vector>

namespace tzu {

class Heatmap {
public:
    Heatmap(int cols = 1024, int rows = 320) : cols_(cols), rows_(rows), pix_(size_t(cols) * rows, kBg), midTick_(cols, 0) {}

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int head() const { return head_; }                       // next column to be written == oldest column
    int64_t baseTick() const { return base_; }               // price (in ticks) of row 0 (bottom)
    int64_t tickPx() const { return tick_; }
    double tickSize() const { return tick_ * 1e-9; }
    const uint32_t* column(int x) const { return &pix_[size_t(x) * rows_]; }   // RGBA8, rows_ pixels, row0 = lowest price
    int64_t midTickAt(int x) const { return midTick_[x]; }

    void setTick(int64_t tickPx) { if (tickPx > 0 && tickPx != tick_) { tick_ = tickPx; base_ = INT64_MIN; } }

    // columns written since last call (UI uploads these); fullDirty => re-upload everything
    std::vector<int> takePending(bool& fullDirty) { fullDirty = fullDirty_; fullDirty_ = false; auto p = std::move(pending_); pending_.clear(); return p; }

    void sample(const Book& b) {
        if (!b.hasBoth() || tick_ <= 0) return;
        const int64_t midT = (b.s[0][0].px + b.s[1][0].px) / 2 / tick_;
        if (base_ == INT64_MIN || midT < base_ + rows_ / 5 || midT > base_ + rows_ * 4 / 5) recenter(midT);

        uint32_t* col = &pix_[size_t(head_) * rows_];
        for (int r = 0; r < rows_; ++r) col[r] = kBg;
        int32_t colMax = 1;
        for (int sd = 0; sd < 2; ++sd) for (int i = 0; i < b.n[sd]; ++i) colMax = std::max(colMax, b.s[sd][i].qty);
        peak_ = std::max(peak_ * 0.9995f, float(colMax));
        for (int sd = 0; sd < 2; ++sd)
            for (int i = 0; i < b.n[sd]; ++i) {
                int64_t r = b.s[sd][i].px / tick_ - base_;
                if (r < 0 || r >= rows_) continue;
                float t = std::sqrt(float(b.s[sd][i].qty) / peak_);       // sqrt tames whales
                col[r] = color(sd, t);
            }
        midTick_[head_] = midT;
        pending_.push_back(head_);
        head_ = (head_ + 1) % cols_;
    }

private:
    static constexpr uint32_t kBg = 0xFF0E0A0A;               // ABGR (little-endian RGBA bytes: 0A 0A 0E FF)
    static uint32_t color(int side, float t) {
        t = std::min(1.f, std::max(0.f, t));
        // bid: deep blue -> cyan-white ; ask: deep magenta -> hot pink/white
        float r, g, b;
        if (side == 0) { r = 20 + 120 * t * t; g = 40 + 190 * t; b = 90 + 165 * t; }
        else           { r = 90 + 165 * t;     g = 15 + 110 * t * t; b = 60 + 120 * t; }
        return 0xFF000000u | (uint32_t(std::min(255.f, b)) << 16) | (uint32_t(std::min(255.f, g)) << 8) | uint32_t(std::min(255.f, r));
    }
    void recenter(int64_t midT) {
        base_ = midT - rows_ / 2;
        std::fill(pix_.begin(), pix_.end(), kBg);
        fullDirty_ = true; pending_.clear();
    }
    int cols_, rows_, head_ = 0;
    int64_t tick_ = 250000000, base_ = INT64_MIN;
    float peak_ = 1.f;
    bool fullDirty_ = true;
    std::vector<uint32_t> pix_;
    std::vector<int64_t> midTick_;
    std::vector<int> pending_;
};

}  // namespace tzu
