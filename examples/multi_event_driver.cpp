#include "multi_event.h"

int main() {
    dsl_runtime::unit<PriceTracker> tracker;
    const QuoteEvent quote{100.0, 104.0};
    auto quoted = tracker.on_event(prepare_PriceTracker_context(quote), quote);
    auto traded = tracker.on_event({}, TradeEvent{103.0, true});
    auto rejected = tracker.on_event({}, TradeEvent{999.0, false});
    return quoted && quoted->result == 102.0 && traded && traded->result && !rejected &&
                   tracker.state().mid == 102.0 && tracker.state().last_trade == 103.0
               ? 0
               : 1;
}
