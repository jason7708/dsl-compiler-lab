#include "accumulate.generated.h"
#include <print>

// The host owns market data and chooses how to read it.
double market_bid = 100.0;
double market_ask = 104.0;
int reads = 0;
double get_bid(int) {
    ++reads;
    return market_bid;
}
double get_ask(int) {
    ++reads;
    return market_ask;
}

int main() {
    dsl_runtime::unit<accumulate_mid> calculation;
    const MarketEvent event{.instrument_id = 7, .enabled = true};
    const auto first = calculation.on_event(prepare_accumulate_mid_context(event), event);
    if (!first || first->result.total != 102.0)
        return 1;
    const auto second = calculation.on_event(prepare_accumulate_mid_context(event), event);
    if (!second || second->result.total != 204.0 || second->result.average != 102.0)
        return 2;
    std::println("total = {}, average = {}", second->result.total, second->result.average);

    const auto failed = calculation.on_event(prepare_accumulate_mid_context(event), event);
    if (failed || failed.error().code != 2 || calculation.state().accumulator_total != 204.0 ||
        calculation.state().accumulator_count != 2.0)
        return 3;
    std::println("error = {}, retained total = {}", failed.error().code,
                 calculation.state().accumulator_total);

    market_ask = 99.0;
    const auto invalid = calculation.on_event(prepare_accumulate_mid_context(event), event);
    if (invalid || invalid.error().code != 1 || calculation.state().accumulator_total != 204.0 ||
        reads != 8)
        return 4;
    std::println("invalid quote = {}, provider reads = {}", invalid.error().code, reads);
}
