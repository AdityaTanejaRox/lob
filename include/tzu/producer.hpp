// >>> THIS is the file you call from your existing MDP3 decoder. <<<
// Header-only, no virtuals, ~20 ns per call. If the ring is full the event is dropped
// (counted in Ring::dropped) - the colo hot path never blocks on the UI.
//
// Mapping from your SBE messages (field names from templates_FixBinary.xml, schema v13):
//
//  MDIncrementalRefreshBook46, per MDEntry:
//     MDEntryType '0'->side 0 (bid), '1'->side 1 (ask), 'J' -> p.reset(secId). Skip 'E'/'F' (implied).
//     p.book(TransactTime, SecurityID, side, MDUpdateAction, MDPriceLevel, MDEntryPx, MDEntrySize,
//            NumberOfOrders, MatchEventIndicator & END_OF_EVENT)
//  MDIncrementalRefreshTradeSummary48, per MDEntry (one entry per price level hit):
//     p.trade(TransactTime, SecurityID, MDEntryPx, MDEntrySize, NumberOfOrders, AggressorSide,
//             p.newGroup())           <-- call newGroup() ONCE per 48 message, reuse for its entries
//  MDInstrumentDefinitionFuture54 : p.future(SecurityID, MinPriceIncrement)
//  MDInstrumentDefinitionOption55 : p.option(SecurityID, UnderlyingSecurityID, PutOrCall==1,
//                                   StrikePrice, expiryEpoch, MinPriceIncrement, OpenInterestQty)
//        expiryEpoch = tzu::expiryEpoch(y, m, d, 20)  (MaturityMonthYear gives y/m, day from your
//        security-group calendar; 20 = 15:00 CDT in UTC, use 21 in winter)
//  Stats (49/51 entries): MDEntryType 'C' (OpenInterest) -> p.stat(sec, StOpenInterest, size)
//                         '6' settle -> StSettle, '4' open -> StOpen (price mantissa)
//  Snapshots (52): replay each level as p.book(..., action=5 /*Overlay*/, ...) after p.reset(sec).
#pragma once
#include "ring.hpp"

namespace tzu {

inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2; const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
inline int64_t expiryEpoch(int y, int m, int d, int hourUtc = 20) { return daysFromCivil(y, m, d) * 86400 + hourUtc * 3600; }

class Producer {
public:
    explicit Producer(Ring& r) : r_(r) {}
    uint32_t newGroup() { return ++group_; }

    void book(uint64_t ts, int32_t sec, uint8_t side, uint8_t action, uint8_t level, int64_t px, int32_t qty, int32_t orders, bool eoe = false) {
        Event e; e.ts = ts; e.secId = sec; e.type = EvType::Book;
        e.b = BookP{px, qty, orders, side, action, level, uint8_t(eoe)}; r_.tryPush(e);
    }
    void reset(uint64_t ts, int32_t sec) { Event e; e.ts = ts; e.secId = sec; e.type = EvType::Reset; r_.tryPush(e); }
    void trade(uint64_t ts, int32_t sec, int64_t px, int32_t qty, int32_t orders, uint8_t aggr, uint32_t group) {
        Event e; e.ts = ts; e.secId = sec; e.type = EvType::Trade; e.t = TradeP{px, qty, orders, group, aggr}; r_.tryPush(e);
    }
    void future(int32_t sec, int64_t tickPx) {
        Event e; e.secId = sec; e.type = EvType::Def; e.d = DefP{0, 0, tickPx, 0, 0, KindFuture}; r_.tryPush(e);
    }
    void option(int32_t sec, int32_t underlying, bool isCall, int64_t strikePx, int64_t expiryEpochS, int64_t tickPx, int32_t oi) {
        Event e; e.secId = sec; e.type = EvType::Def;
        e.d = DefP{strikePx, expiryEpochS, tickPx, underlying, oi, uint8_t(isCall ? KindCall : KindPut)}; r_.tryPush(e);
    }
    void stat(int32_t sec, StatKind k, int64_t value) { Event e; e.secId = sec; e.type = EvType::Stat; e.s = StatP{value, uint8_t(k)}; r_.tryPush(e); }

private:
    Ring& r_; uint32_t group_ = 0;
};

}  // namespace tzu
