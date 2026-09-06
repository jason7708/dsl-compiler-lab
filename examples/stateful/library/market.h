#pragma once
struct MarketEvent {
    int instrument_id;
    bool enabled;
};
double get_bid(int instrument_id);
double get_ask(int instrument_id);
