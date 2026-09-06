#pragma once
#include "market.h"

double mid_price(ext_event event) {
    double bid = get_bid(event.instrument_id);
    double ask = get_ask(event.instrument_id);
    double mid = (bid + ask) / 2;
    return mid;
}
