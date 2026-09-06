#include <print>

double compute(double a, double b);

int main() {
    const double result = compute(3.0, 4.0);
    std::println("{}", result);
    return result == 5.0 && compute(0.0, 0.0) == 0.0 ? 0 : 1;
}
