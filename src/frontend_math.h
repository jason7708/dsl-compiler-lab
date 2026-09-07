#pragma once

#include "frontend_ir.h"
#include <array>
#include <optional>
#include <string_view>

namespace dsl::source {
struct MathBuiltin {
    MathFunction function;
    std::string_view name;
    std::size_t arity;
};

// C++ DSL surface declarations only; common OP semantics live in registry.h.
inline constexpr std::array mathBuiltins{
    MathBuiltin{MathFunction::Abs, "abs", 1},     MathBuiltin{MathFunction::Sqrt, "sqrt", 1},
    MathBuiltin{MathFunction::Pow, "pow", 2},     MathBuiltin{MathFunction::Exp, "exp", 1},
    MathBuiltin{MathFunction::Log, "log", 1},     MathBuiltin{MathFunction::Sin, "sin", 1},
    MathBuiltin{MathFunction::Cos, "cos", 1},     MathBuiltin{MathFunction::Tan, "tan", 1},
    MathBuiltin{MathFunction::Min, "min", 2},     MathBuiltin{MathFunction::Max, "max", 2},
    MathBuiltin{MathFunction::Floor, "floor", 1}, MathBuiltin{MathFunction::Ceil, "ceil", 1},
    MathBuiltin{MathFunction::Round, "round", 1},
};

[[nodiscard]] const MathBuiltin &mathBuiltin(MathFunction function);
[[nodiscard]] std::optional<MathFunction> findMathBuiltin(std::string_view name);
} // namespace dsl::source
