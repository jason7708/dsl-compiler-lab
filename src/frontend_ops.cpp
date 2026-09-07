#include "frontend_math.h"
#include <algorithm>
#include <stdexcept>
namespace dsl::source {
std::string_view opName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:
        return "add";
    case BinaryOp::Subtract:
        return "sub";
    case BinaryOp::Multiply:
        return "mul";
    case BinaryOp::Divide:
        return "div";
    case BinaryOp::Less:
        return "lt";
    case BinaryOp::LessEqual:
        return "le";
    case BinaryOp::Greater:
        return "gt";
    case BinaryOp::GreaterEqual:
        return "ge";
    case BinaryOp::Equal:
        return "eq";
    case BinaryOp::NotEqual:
        return "ne";
    }
    throw std::logic_error("invalid IR operator");
}
const MathBuiltin &mathBuiltin(MathFunction function) {
    const auto builtin = std::ranges::find(mathBuiltins, function, &MathBuiltin::function);
    if (builtin == mathBuiltins.end())
        throw std::logic_error("invalid math function");
    return *builtin;
}
std::optional<MathFunction> findMathBuiltin(std::string_view name) {
    const auto builtin = std::ranges::find(mathBuiltins, name, &MathBuiltin::name);
    if (builtin == mathBuiltins.end())
        return std::nullopt;
    return builtin->function;
}
} // namespace dsl::source
