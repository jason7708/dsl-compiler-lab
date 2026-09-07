#include "dsl/registry.h"
#include <algorithm>
#include <array>
namespace dsl::ir {
std::span<const OpDefinition> registry() {
    using enum Rule;
    using enum Effect;
    static constexpr std::array definitions{
        OpDefinition{OpCode::Constant, "constant", Constant, None},
        OpDefinition{OpCode::Identity, "identity", Identity, None},
        OpDefinition{OpCode::Add, "add", FloatBinary, FloatingEnvironment},
        OpDefinition{OpCode::Subtract, "sub", FloatBinary, FloatingEnvironment},
        OpDefinition{OpCode::Multiply, "mul", FloatBinary, FloatingEnvironment},
        OpDefinition{OpCode::Divide, "div", FloatBinary, FloatingEnvironment},
        OpDefinition{OpCode::Positive, "positive", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Negate, "negate", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Not, "not", BoolUnary, None},
        OpDefinition{OpCode::Less, "lt", Compare, FloatingEnvironment},
        OpDefinition{OpCode::LessEqual, "le", Compare, FloatingEnvironment},
        OpDefinition{OpCode::Greater, "gt", Compare, FloatingEnvironment},
        OpDefinition{OpCode::GreaterEqual, "ge", Compare, FloatingEnvironment},
        OpDefinition{OpCode::Equal, "eq", Compare, FloatingEnvironment},
        OpDefinition{OpCode::NotEqual, "ne", Compare, FloatingEnvironment},
        OpDefinition{OpCode::Abs, "abs", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Sqrt, "sqrt", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Pow, "pow", FloatBinary, FloatingEnvironment},
        OpDefinition{OpCode::Exp, "exp", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Log, "log", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Sin, "sin", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Cos, "cos", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Tan, "tan", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Min, "min", FloatBinary, FloatingEnvironment},
        OpDefinition{OpCode::Max, "max", FloatBinary, FloatingEnvironment},
        OpDefinition{OpCode::Floor, "floor", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Ceil, "ceil", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Round, "round", FloatUnary, FloatingEnvironment},
        OpDefinition{OpCode::Aggregate, "aggregate", Aggregate, None},
        OpDefinition{OpCode::Extract, "extract", Extract, None},
        OpDefinition{OpCode::Insert, "insert", Insert, None},
        OpDefinition{OpCode::Call, "call", Call, Derived},
        OpDefinition{OpCode::ExternalCall, "external_call", ExternalCall, External},
        OpDefinition{OpCode::If, "if", If, Derived},
        OpDefinition{OpCode::Scope, "scope", Scope, Derived},
        OpDefinition{OpCode::Evaluate, "evaluate", Evaluate, Derived}};
    return definitions;
}
const OpDefinition *lookup(OpCode code, std::span<const OpDefinition> definitions) {
    auto found = std::ranges::find(definitions, code, &OpDefinition::code);
    return found == definitions.end() ? nullptr : &*found;
}
} // namespace dsl::ir
