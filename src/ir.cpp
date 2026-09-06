#include "dsl/ir.h"
#include "dsl/math.h"
#include "dsl/visit.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace dsl {
std::string_view typeName(Type type) {
    switch (type) {
    case Type::Double:
        return "double";
    case Type::Bool:
        return "bool";
    case Type::Int:
        return "int";
    case Type::Record:
        return "record";
    }
    throw std::logic_error("invalid IR type");
}
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
std::string_view opSymbol(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Subtract:
        return "-";
    case BinaryOp::Multiply:
        return "*";
    case BinaryOp::Divide:
        return "/";
    case BinaryOp::Less:
        return "<";
    case BinaryOp::LessEqual:
        return "<=";
    case BinaryOp::Greater:
        return ">";
    case BinaryOp::GreaterEqual:
        return ">=";
    case BinaryOp::Equal:
        return "==";
    case BinaryOp::NotEqual:
        return "!=";
    }
    throw std::logic_error("invalid IR operator");
}
std::string_view opName(UnaryOp op) {
    switch (op) {
    case UnaryOp::Plus:
        return "pos";
    case UnaryOp::Negate:
        return "neg";
    case UnaryOp::Not:
        return "not";
    }
    throw std::logic_error("invalid IR operator");
}
char opSymbol(UnaryOp op) {
    switch (op) {
    case UnaryOp::Plus:
        return '+';
    case UnaryOp::Negate:
        return '-';
    case UnaryOp::Not:
        return '!';
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
std::string doubleLiteral(double value) {
    return std::format("{}0x{:a}", std::signbit(value) ? "-" : "", std::abs(value));
}
namespace {
std::string formatRegion(const Module &module, const Function &function, const Region &region,
                         const std::string &indent, std::string_view terminator) {
    std::string text;
    for (const auto id : region.instructions) {
        const auto &value = function.values[id];
        const auto operation = std::visit(
            Overloaded{
                [](const Parameter &parameter) {
                    return parameter.recordName.empty()
                               ? std::format("param {}", parameter.name)
                               : std::format("param {} ({})", parameter.name, parameter.recordName);
                },
                [](const IntegerConstant &constant) {
                    return std::format("constant {}", constant.value);
                },
                [](const Member &member) {
                    return std::format("member %{}.{}", member.base, member.field);
                },
                [&](const ContextRead &read) {
                    std::string arguments;
                    for (const auto argument : read.arguments) {
                        if (!arguments.empty())
                            arguments += ", ";
                        arguments += std::format("%{}", argument);
                    }
                    return std::format("context {} <- @{}({})", read.field,
                                       module.externals[read.external].name, arguments);
                },
                [](const Constant &constant) {
                    return std::format("constant {}", doubleLiteral(constant.value));
                },
                [](const BooleanConstant &constant) {
                    return std::format("constant {}", constant.value);
                },
                [](const Binary &binary) {
                    return std::format("{} %{}, %{}", opName(binary.op), binary.lhs, binary.rhs);
                },
                [](const Unary &unary) {
                    return std::format("{} %{}", opName(unary.op), unary.operand);
                },
                [](const Reference &reference) {
                    return std::format("ref %{} ({})", reference.target, reference.name);
                },
                [&](const Call &call) {
                    const auto callee =
                        std::visit(Overloaded{
                                       [](MathFunction function) {
                                           return std::string(mathBuiltin(function).cppName);
                                       },
                                       [&](ExternalFunction function) {
                                           return module.externals[function.id].name;
                                       },
                                       [&](DslFunction function) {
                                           return module.functions[function.id].name;
                                       },
                                   },
                                   call.target);
                    auto text = std::format("call @{}(", callee);
                    bool first = true;
                    for (const auto argument : call.arguments) {
                        if (!first)
                            text += ", ";
                        first = false;
                        text += std::format("%{}", argument);
                    }
                    return text + ')';
                },
                [&](const RecordInit &op) {
                    std::string fields;
                    for (auto field : op.fields) {
                        if (!fields.empty())
                            fields += ", ";
                        fields += std::format("%{}", field);
                    }
                    return "record {" + fields + "}";
                },
                [](const Store &op) {
                    return std::format("store %{} <- %{}", op.target, op.value);
                },
                [](const FieldStore &op) {
                    return std::format("store %{}.{} <- %{}", op.base, op.field, op.value);
                },
                [](const StateCopy &op) { return std::format("state_copy %{}", op.source); },
                [](const Return &op) {
                    return std::format("{} %{}", op.failure ? "error" : "return", op.value);
                },
                [&](const Scope &op) {
                    return "scope {\n" +
                           formatRegion(module, function, op.body, indent + "  ", "") + indent +
                           "}";
                },
                [&](const If &op) {
                    return std::format(
                        "if %{} {{\n{}{}}} else {{\n{}{}}}", op.condition,
                        formatRegion(module, function, op.whenTrue, indent + "  ", ""), indent,
                        formatRegion(module, function, op.whenFalse, indent + "  ", ""), indent);
                },
                [&](const Invoke &op) {
                    return std::format("invoke @{} {{\n{}{}}}", module.functions[op.function].name,
                                       formatRegion(module, function, op.body, indent + "  ", ""),
                                       indent);
                },
                [&](const Select &select) {
                    return std::format(
                        "select %{} {{\n{}  then {{\n{}{}}}\n{}  else {{\n{}{}}}\n{}}}",
                        select.condition, indent,
                        formatRegion(module, function, select.whenTrue, indent + "    ", "yield"),
                        indent + "  ", indent,
                        formatRegion(module, function, select.whenFalse, indent + "    ", "yield"),
                        indent + "  ", indent);
                },
            },
            value.operation);
        text += std::format("{}%{} : {} = {}\n", indent, id, typeName(value.type), operation);
    }
    if (!terminator.empty())
        text += std::format("{}{} %{}\n", indent, terminator, region.result);
    return text;
}
} // namespace
std::string formatIR(const Module &module) {
    std::string text;
    for (const auto &external : module.externals) {
        std::string parameters;
        for (std::size_t i = 0; i < external.arity; ++i)
            parameters += std::format("{}{}", i == 0 ? "" : ", ", typeName(external.parameters[i]));
        text += std::format("extern @{}({}) -> double\n", external.name, parameters);
    }
    for (const auto &function : module.functions) {
        text += std::format(
            "func @{} -> {} {{\n{}}}\n", function.name, typeName(function.returnType),
            formatRegion(module, function, function.body, "  ", module.objectMode ? "" : "return"));
    }
    return text;
}
} // namespace dsl
