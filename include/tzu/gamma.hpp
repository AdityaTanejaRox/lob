// Black-76 (options on futures - what CME ES options are) pricing, IV solver, greeks.
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace tzu {

inline double npdf(double x) { return 0.3989422804014327 * std::exp(-0.5 * x * x); }
inline double ncdf(double x) { return 0.5 * std::erfc(-x * 0.7071067811865476); }

struct B76 {
    static double d1(double F, double K, double T, double v) { return (std::log(F / K) + 0.5 * v * v * T) / (v * std::sqrt(T)); }
    static double price(bool call, double F, double K, double T, double v, double r) {
        const double df = std::exp(-r * T), sd = v * std::sqrt(T), a = d1(F, K, T, v), b = a - sd;
        return call ? df * (F * ncdf(a) - K * ncdf(b)) : df * (K * ncdf(-b) - F * ncdf(-a));
    }
    static double gamma(double F, double K, double T, double v, double r) {
        return std::exp(-r * T) * npdf(d1(F, K, T, v)) / (F * v * std::sqrt(T));
    }
    static double delta(bool call, double F, double K, double T, double v, double r) {
        const double df = std::exp(-r * T), a = d1(F, K, T, v);
        return call ? df * ncdf(a) : -df * ncdf(-a);
    }
    // bisection; NaN if the price is outside no-arbitrage bounds
    static double iv(bool call, double px, double F, double K, double T, double r) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        if (px <= 0 || T <= 0 || F <= 0 || K <= 0) return nan;
        double lo = 1e-4, hi = 4.0;
        if (px < price(call, F, K, T, lo, r) || px > price(call, F, K, T, hi, r)) return nan;
        for (int i = 0; i < 48; ++i) {
            double m = 0.5 * (lo + hi);
            (price(call, F, K, T, m, r) < px ? lo : hi) = m;
        }
        return 0.5 * (lo + hi);
    }
};

struct GexRow { double strike = 0, callGex = 0, putGex = 0, net = 0, callOI = 0, putOI = 0; };

struct GexProfile {
    bool valid = false;
    double spot = 0, netGex = 0;            // $ delta change per 1% move, dealers' side
    double flip = 0, callWall = 0, accel = 0, em = 0, atmIv = 0, maxAbs = 1;
    std::vector<GexRow> rows;               // ascending strike
};

}  // namespace tzu
