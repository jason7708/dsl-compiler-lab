#include "pricing.dsl.h"

double price_difference(ext_event event) {
    double mid = mid_price(event);
    return mid - event.reference_price;
}
