#include "prices.generated.h"

#include <print>

int main() {
    const ext_event event{.instrument_id = 0, .reference_price = 99.0};
    const auto ctx = prepare_price_difference_context(event);
    dsl_runtime::unit<price_difference> calculation;
    const auto result = calculation.on_event(ctx, event);
    if (!result)
        return 1;
    std::println("difference = {}", result->result);

    // A caller can supply context directly, with no external reads.
    const auto manual = mid_price{}(mid_price_context{.bid = 10.0, .ask = 14.0}, {}, event);
    if (!manual)
        return 2;
    std::println("manual mid = {}", manual->result);
    return result->result == 2.0 && manual->result == 12.0 ? 0 : 3;
}
