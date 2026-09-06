#include <api.h>
#include <dsl_runtime/math.h>

double score(double x, double y) { return external_ops::weighted_sum(x, y); }

double compute(double x, double y) { return dsl_math::sqrt(score(x, y)); }
