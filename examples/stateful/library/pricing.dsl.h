#pragma once
#include "market.h"

struct Quote {
    double mid;
    double spread;
};
struct Ledger {
    double total;
    double count;
};
struct Totals {
    double total;
    double average;
};
struct CalcError {
    int code;
};

Quote read_quote(MarketEvent event) {
    double bid = get_bid(event.instrument_id);
    double ask = get_ask(event.instrument_id);
    if (!event.enabled || bid < 0.0 || ask < bid) {
        throw CalcError{.code = 1};
    }
    return Quote{.mid = (bid + ask) / 2, .spread = ask - bid};
}

Totals accumulate_value(double value, Ledger& state) {
    state.total = state.total + value;
    state.count = state.count + 1.0;
    if (state.total > 250.0) {
        throw CalcError{.code = 2};
    }
    return Totals{.total = state.total, .average = state.total / state.count};
}
