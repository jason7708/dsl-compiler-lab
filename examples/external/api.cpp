#include "api.h"

#include <array>

namespace external_ops {
double weighted_sum(double x, double y) {
    const auto terms = std::array{x * 3.0, y * 4.0};
    double result = 0.0;
    for (const auto term : terms)
        result += term;
    return result;
}
} // namespace external_ops
