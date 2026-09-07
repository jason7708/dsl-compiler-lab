#pragma once
#include "market.h"

struct Quote {
    double mid;
    double spread;
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

struct accumulate_value {
    double total = 0.0;
    double count = 0.0;

    Totals operator()(double value) {
        total = total + value;
        count = count + 1.0;
        if (total > 250.0) {
            throw CalcError{.code = 2};
        }
        return Totals{.total = total, .average = total / count};
    }
};
