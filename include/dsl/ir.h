#pragma once
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dsl::ir {
// IDs remain stable within a module; importing/serializing modules must remap IDs.
template <class Tag> struct Id {
    std::uint32_t value;
    auto operator<=>(const Id &) const = default;
};
using TypeId = Id<struct TypeTag>;
using ValueId = Id<struct ValueTag>;
using OperationId = Id<struct OperationTag>;
using FunctionId = Id<struct FunctionTag>;
using ExternalId = Id<struct ExternalTag>;
using ConstantId = Id<struct ConstantTag>;
using FieldId = Id<struct FieldTag>;

enum class Signedness { Signed, Unsigned };
enum class FloatFormat { IEEE754Binary32, IEEE754Binary64 };
struct BoolType {};
struct IntegerType {
    unsigned width;
    Signedness signedness;
};
struct FloatType {
    FloatFormat format;
};
struct Field {
    FieldId id;
    TypeId type;
};
struct RecordType {
    std::vector<Field> fields;
};
using Type = std::variant<BoolType, IntegerType, FloatType, RecordType>;
struct Constant {
    TypeId type;
    // Exact numeric bits (including signed zero); aggregates reference constants.
    std::variant<std::uint64_t, std::vector<ConstantId>> payload;
};

// All expression/call/control operations use the same registered representation.
enum class OpCode {
    Constant,
    Identity,
    Add,
    Subtract,
    Multiply,
    Divide,
    Positive,
    Negate,
    Not,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    Abs,
    Sqrt,
    Pow,
    Exp,
    Log,
    Sin,
    Cos,
    Tan,
    Min,
    Max,
    Floor,
    Ceil,
    Round,
    Aggregate,
    Extract,
    Insert,
    Call,
    ExternalCall,
    If,
    Scope,
    Evaluate
};
struct Evaluation {
    std::optional<TypeId> error;
};
using Attribute =
    std::variant<std::monostate, ConstantId, FieldId, FunctionId, ExternalId, Evaluation>;
struct ReturnSuccess {
    std::vector<ValueId> values;
};
struct ReturnError {
    ValueId error;
};
struct Yield {
    std::vector<ValueId> values;
};
struct Unreachable {};
using Terminator = std::variant<ReturnSuccess, ReturnError, Yield, Unreachable>;
struct Operation;
struct Region {
    std::vector<Operation> operations;
    std::optional<Terminator> terminator;
    // Diagnostic metadata, independent of executable semantics and target layout.
    std::string location = {};
    std::string terminatorLocation = {};
};
struct Operation {
    OperationId id;
    OpCode code;
    std::vector<ValueId> operands;
    std::vector<ValueId> results;
    Attribute attribute;
    std::vector<Region> regions;
    std::string location;
};
struct Signature {
    std::vector<TypeId> parameters;
    std::vector<TypeId> results;
    std::optional<TypeId> error;
};
struct Function {
    std::string debugName;
    Signature signature;
    // Values are typed independently from operations; parameters also define values.
    std::vector<TypeId> values;
    std::vector<ValueId> parameters;
    Region body;
};
struct External {
    std::string debugName;
    Signature signature;
};
struct Module {
    std::vector<Type> types;
    std::vector<Constant> constants;
    std::vector<External> externals;
    std::vector<Function> functions;
};
[[nodiscard]] std::string format(const Module &);
} // namespace dsl::ir
