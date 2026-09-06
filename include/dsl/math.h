#pragma once

#include "dsl/ir.h"
#include <array>
#include <optional>
#include <string_view>

namespace dsl {
struct MathBuiltin {
    MathFunction function;
    std::string_view name;
    std::string_view cppName;
    std::size_t arity;
};

// Registry validates the real runtime API and selects its generated call target.
inline constexpr std::array mathBuiltins{
    MathBuiltin{MathFunction::Abs, "abs", "dsl_math::abs", 1},
    MathBuiltin{MathFunction::Sqrt, "sqrt", "dsl_math::sqrt", 1},
    MathBuiltin{MathFunction::Pow, "pow", "dsl_math::pow", 2},
    MathBuiltin{MathFunction::Exp, "exp", "dsl_math::exp", 1},
    MathBuiltin{MathFunction::Log, "log", "dsl_math::log", 1},
    MathBuiltin{MathFunction::Sin, "sin", "dsl_math::sin", 1},
    MathBuiltin{MathFunction::Cos, "cos", "dsl_math::cos", 1},
    MathBuiltin{MathFunction::Tan, "tan", "dsl_math::tan", 1},
    MathBuiltin{MathFunction::Min, "min", "dsl_math::min", 2},
    MathBuiltin{MathFunction::Max, "max", "dsl_math::max", 2},
    MathBuiltin{MathFunction::Floor, "floor", "dsl_math::floor", 1},
    MathBuiltin{MathFunction::Ceil, "ceil", "dsl_math::ceil", 1},
    MathBuiltin{MathFunction::Round, "round", "dsl_math::round", 1},
};

[[nodiscard]] const MathBuiltin &mathBuiltin(MathFunction function);
[[nodiscard]] std::optional<MathFunction> findMathBuiltin(std::string_view name);
} // namespace dsl
