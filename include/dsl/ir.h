#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dsl {

enum class Type { Double, Bool, Int, Record };
using ValueId = std::size_t;
using FunctionId = std::size_t;
enum class BinaryOp {
    Add,
    Subtract,
    Multiply,
    Divide,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual
};
enum class UnaryOp { Plus, Negate, Not };
enum class MathFunction { Abs, Sqrt, Pow, Exp, Log, Sin, Cos, Tan, Min, Max, Floor, Ceil, Round };

struct Parameter {
    std::string name;
    std::string recordName = {};
    bool state = false;
};
struct IntegerConstant {
    int value;
};
struct Member {
    ValueId base;
    std::string field;
};
struct Constant {
    double value;
};
struct BooleanConstant {
    bool value;
};
struct Binary {
    BinaryOp op;
    ValueId lhs;
    ValueId rhs;
};
struct Unary {
    UnaryOp op;
    ValueId operand;
};
struct Reference {
    ValueId target;
    std::string name;
};
struct DslFunction {
    FunctionId id;
};
struct ExternalFunction {
    std::size_t id;
};
struct ExternalDeclaration {
    std::string name;
    std::size_t arity;
    std::vector<Type> parameters;
    bool context = false;
};
using CallTarget = std::variant<MathFunction, DslFunction, ExternalFunction>;
struct Call {
    CallTarget target;
    std::vector<ValueId> arguments;
    std::string location = {};
};

struct ContextRead {
    std::size_t external;
    std::vector<ValueId> arguments;
    std::string field;
};
struct RecordField {
    std::string name;
    Type type;
};
struct RecordDeclaration {
    std::string name;
    std::vector<RecordField> fields;
    bool external = true;
};

// Value IDs are local to a function; each region owns its executed instructions.
// A select evaluates only its chosen region, then yields that region's result.
struct Region {
    std::vector<ValueId> instructions;
    ValueId result = 0;
};
struct Select {
    ValueId condition;
    Region whenTrue;
    Region whenFalse;
};

struct RecordInit {
    std::vector<ValueId> fields;
};
struct Store {
    ValueId target;
    ValueId value;
};
struct FieldStore {
    ValueId base;
    std::string field;
    ValueId value;
};
struct Return {
    ValueId value;
    bool failure = false;
};
struct If {
    ValueId condition;
    Region whenTrue;
    Region whenFalse;
};
struct Scope {
    Region body;
};
struct StateCopy {
    ValueId source;
};
struct Invoke {
    FunctionId function;
    Region body;
    std::optional<ValueId> state;
    std::optional<ValueId> callerState;
};

using Operation = std::variant<Parameter, Constant, BooleanConstant, IntegerConstant, Member,
                               Binary, Unary, Reference, Call, Select, ContextRead, RecordInit,
                               Store, FieldStore, Return, If, Scope, StateCopy, Invoke>;
struct Value {
    Type type = Type::Double;
    Operation operation;
    std::string recordName = {};
};
struct Function {
    std::string name = "compute";
    Type returnType = Type::Double;
    std::vector<Value> values;
    Region body;
    std::string location = {};
    std::string returnRecord = {};
    std::string stateRecord = {};
    std::string errorRecord = {};
    std::optional<ValueId> stateParameter = {};
};

struct Module {
    std::vector<Function> functions;
    FunctionId entry = 0;
    std::vector<ExternalDeclaration> externals;
    std::vector<std::string> externalHeaders;
    std::vector<std::string> externalMacros;
    std::vector<RecordDeclaration> records;
    std::vector<std::string> importedNames;
    bool objectMode = false;
};

[[nodiscard]] std::string_view typeName(Type type);
[[nodiscard]] std::string_view opName(BinaryOp op);
[[nodiscard]] std::string_view opSymbol(BinaryOp op);
[[nodiscard]] std::string_view opName(UnaryOp op);
[[nodiscard]] char opSymbol(UnaryOp op);
[[nodiscard]] std::string doubleLiteral(double value);
[[nodiscard]] std::string formatIR(const Module &module);

} // namespace dsl
