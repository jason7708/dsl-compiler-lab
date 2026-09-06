#include <dsl_runtime/math.h>

double square(double x) { return x * x; }

double length(double a, double b) { return dsl_math::sqrt(square(a) + square(b)); }

double compute(double a, double b) { return length(a, b); }
