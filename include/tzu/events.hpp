// Wire format between your MDP3 decoder (producer) and the dashboard (consumer).
// Fixed 64-byte POD so it can live in shared memory. Prices are the raw SBE PRICE9
// mantissa (exponent -9, see your templates_FixBinary.xml) so the decoder does no float math.
#pragma once
#include <cstdint>
#include <climits>

namespace tzu {

constexpr int64_t kNullPx = INT64_MIN;                 // PRICENULL9 null
inline double px2d(int64_t m) { return double(m) * 1e-9; }
inline int64_t d2px(double d) { return int64_t(d * 1e9 + (d >= 0 ? 0.5 : -0.5)); }

enum class EvType : uint8_t { Book = 1, Trade = 2, Def = 3, Stat = 4, Reset = 5 };
enum StatKind : uint8_t { StOpenInterest = 1, StSettle = 2, StOpen = 3, StClearedVol = 4 };
enum InstKind : uint8_t { KindFuture = 0, KindCall = 1, KindPut = 2 };

struct BookP  { int64_t px; int32_t qty; int32_t orders; uint8_t side /*0 bid,1 ask*/, action /*MDUpdateAction 0..5*/, level /*1-based*/, endOfEvent; };
struct TradeP { int64_t px; int32_t qty; int32_t orders; uint32_t group /*same value for entries of one MDP packet/message*/; uint8_t aggr /*0 none,1 buy,2 sell*/; };
struct DefP   { int64_t strike; int64_t expiryEpochS; int64_t tick; int32_t underlying; int32_t oi; uint8_t kind; };
struct StatP  { int64_t value; uint8_t kind; };

struct alignas(64) Event {
    uint64_t ts;      // ns since epoch (TransactTime)
    int32_t  secId;
    EvType   type;
    union { BookP b; TradeP t; DefP d; StatP s; };
    Event() : ts(0), secId(0), type(EvType::Reset) { d = DefP{}; }
};
static_assert(sizeof(Event) == 64, "Event must be one cache line");

}  // namespace tzu
