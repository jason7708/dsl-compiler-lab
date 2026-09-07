#pragma once
#include "dsl/ir.h"
#include <span>
#include <string_view>
namespace dsl::ir {
enum class Rule {
    Constant,
    Identity,
    FloatBinary,
    FloatUnary,
    BoolUnary,
    Compare,
    Aggregate,
    Extract,
    Insert,
    Call,
    ExternalCall,
    If,
    Scope,
    Evaluate
};
// Conservative observability: ordered operations cannot be freely moved or deleted.
enum class Effect { None, FloatingEnvironment, External, Derived };
struct OpDefinition {
    OpCode code;
    std::string_view name;
    Rule rule;
    Effect effect;
};
[[nodiscard]] std::span<const OpDefinition> registry();
[[nodiscard]] const OpDefinition *lookup(OpCode, std::span<const OpDefinition>);
} // namespace dsl::ir
