#pragma once

// Public C++ API shared by DSL source and generated kernels.
namespace dsl_math {
double abs(double x);
double sqrt(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double min(double x, double y);
double max(double x, double y);
double floor(double x);
double ceil(double x);
double round(double x);
} // namespace dsl_math
