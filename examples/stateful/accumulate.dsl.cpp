#include "pricing.dsl.h"

struct accumulate_mid {
    accumulate_value accumulator;

    Totals operator()(MarketEvent event) {
        Quote quote = read_quote(event);
        return accumulator(quote.mid);
    }
};
