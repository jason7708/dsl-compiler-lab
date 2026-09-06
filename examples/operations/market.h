#pragma once

struct ext_event {
    int instrument_id;
    double reference_price;
};

double get_bid(int id);
double get_ask(int id);
