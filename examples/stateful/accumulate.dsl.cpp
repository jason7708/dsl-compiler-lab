#include "pricing.dsl.h"

Totals accumulate_mid(MarketEvent event, Ledger& state) {
    Quote quote = read_quote(event);
    return accumulate_value(quote.mid, state);
}
