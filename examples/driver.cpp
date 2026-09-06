#include <iostream>

double compute(double a, double b);

int main() {
    const double result = compute(2.0, 4.0);
    std::cout << result << '\n';
    return result == 3.0 ? 0 : 1;
}