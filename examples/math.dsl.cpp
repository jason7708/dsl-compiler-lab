#include <dsl_runtime/math.h>

double compute(double a, double b) {
    const double radius = dsl_math::sqrt(dsl_math::pow(a, 2.0) + dsl_math::pow(b, 2.0));
    const bool nonzero = radius > 0.0;
    return nonzero ? dsl_math::min(radius, 10.0) : 0.0;
}
