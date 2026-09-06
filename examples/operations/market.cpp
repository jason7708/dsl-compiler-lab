#include "market.h"

#include <array>
#include <stdexcept>

namespace {
struct quote {
    double bid;
    double ask;
};
constexpr std::array quotes{quote{100.0, 102.0}, quote{200.0, 204.0}};
} // namespace

double get_bid(int id) { return quotes.at(id).bid; }
double get_ask(int id) { return quotes.at(id).ask; }
