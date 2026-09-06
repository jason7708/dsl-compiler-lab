#include "dsl_runtime/math.h"

#include <cmath>

namespace dsl_math {
double abs(double x) { return std::abs(x); }
double sqrt(double x) { return std::sqrt(x); }
double pow(double x, double y) { return std::pow(x, y); }
double exp(double x) { return std::exp(x); }
double log(double x) { return std::log(x); }
double sin(double x) { return std::sin(x); }
double cos(double x) { return std::cos(x); }
double tan(double x) { return std::tan(x); }
double min(double x, double y) { return std::fmin(x, y); }
double max(double x, double y) { return std::fmax(x, y); }
double floor(double x) { return std::floor(x); }
double ceil(double x) { return std::ceil(x); }
double round(double x) { return std::round(x); }
} // namespace dsl_math
