#include "dsl/codegen.h"
#include "dsl/math.h"
#include "dsl/visit.h"

#include <format>
#include <ranges>
#include <stdexcept>

namespace dsl {
namespace {
std::string functionName(const Module &module, FunctionId id) {
    return id == module.entry ? "compute" : std::format("dsl_function_{}", id);
}
std::string generateRegion(const Module &module, const Function &function, const Region &region,
                           const std::string &indent) {
    std::string body;
    for (const auto id : region.instructions) {
        const auto &value = function.values[id];
        const auto emit = [&](std::string expression) {
            body +=
                std::format("{}const {} v{} = {};\n", indent, typeName(value.type), id, expression);
        };
        std::visit(
            Overloaded{
                [](const Parameter &) {},
                [](const RecordInit &) {
                    throw std::logic_error("object-only IR in scalar codegen");
                },
                [](const Store &) { throw std::logic_error("object-only IR in scalar codegen"); },
                [](const FieldStore &) {
                    throw std::logic_error("object-only IR in scalar codegen");
                },
                [](const Return &) { throw std::logic_error("object-only IR in scalar codegen"); },
                [](const If &) { throw std::logic_error("object-only IR in scalar codegen"); },
                [](const Scope &) { throw std::logic_error("object-only IR in scalar codegen"); },
                [](const StateCopy &) {
                    throw std::logic_error("object-only IR in scalar codegen");
                },
                [](const Invoke &) { throw std::logic_error("object-only IR in scalar codegen"); },

                [&](const IntegerConstant &constant) { emit(std::format("{}", constant.value)); },
                [&](const Member &member) {
                    emit(std::format("v{}.{}", member.base, member.field));
                },
                [&](const ContextRead &read) { emit("ctx." + read.field); },
                [&](const Constant &constant) { emit(doubleLiteral(constant.value)); },
                [&](const BooleanConstant &constant) { emit(std::format("{}", constant.value)); },
                [&](const Binary &binary) {
                    emit(std::format("(v{} {} v{})", binary.lhs, opSymbol(binary.op), binary.rhs));
                },
                [&](const Unary &unary) {
                    emit(std::format("({}v{})", opSymbol(unary.op), unary.operand));
                },
                [&](const Reference &reference) { emit(std::format("v{}", reference.target)); },
                [&](const Call &call) {
                    const auto callee = std::visit(
                        Overloaded{
                            [](MathFunction function) {
                                return std::string(mathBuiltin(function).cppName);
                            },
                            [&](DslFunction function) { return functionName(module, function.id); },
                            [&](ExternalFunction function) {
                                return "::" + module.externals[function.id].name;
                            },
                        },
                        call.target);
                    auto expression = std::format("{}(", callee);
                    bool first = true;
                    for (const auto argument : call.arguments) {
                        if (!first)
                            expression += ", ";
                        first = false;
                        expression += std::format("v{}", argument);
                    }
                    emit(expression + ')');
                },
                [&](const Select &select) {
                    // Branch instructions stay inside the chosen branch. In particular,
                    // an unselected sqrt/log/division must not affect errno or fenv.
                    emit(std::format(
                        "[&]() -> {} {{\n{}  if (v{}) {{\n{}{}  }} else {{\n{}{}  }}\n{}}}()",
                        typeName(value.type), indent, select.condition,
                        generateRegion(module, function, select.whenTrue, indent + "    "), indent,
                        generateRegion(module, function, select.whenFalse, indent + "    "), indent,
                        indent));
                },
            },
            value.operation);
    }
    body += std::format("{}return v{};\n", indent, region.result);
    return body;
}
std::string signature(const Module &module, FunctionId id) {
    const auto &function = module.functions[id];
    std::string parameters;
    for (const auto [valueId, value] : function.values | std::views::enumerate) {
        if (!std::holds_alternative<Parameter>(value.operation))
            continue;
        if (!parameters.empty())
            parameters += ", ";
        parameters += std::format("{} v{}", typeName(value.type), valueId);
    }
    return std::format("{}{} {}({})", id == module.entry ? "" : "static ",
                       typeName(function.returnType), functionName(module, id), parameters);
}
} // namespace
std::string generateCpp(const Module &module) {
    std::string text = "// Generated from typed DSL IR. C++23, IEEE-754 double.\n"
                       "// Compile with -std=c++23 -fno-fast-math -ffp-contract=off.\n"
                       "// Link with dsl_runtime.\n#include <dsl_runtime/math.h>\n\n";
    for (const auto &header : module.externalHeaders)
        text += std::format("#include \"{}\"\n", header);
    // Interface macros have already served their purpose in the headers. They
    // must not rewrite the identifiers or keywords generated from typed IR.
    for (const auto &macro : module.externalMacros)
        text += std::format("#undef {}\n", macro);
    for (const auto [id, function] : module.functions | std::views::enumerate) {
        text += signature(module, id) + ";\n";
    }
    text += '\n';
    for (const auto [id, function] : module.functions | std::views::enumerate) {
        text += std::format("{} {{\n{}}}\n\n", signature(module, id),
                            generateRegion(module, function, function.body, "  "));
    }
    return text;
}
} // namespace dsl
