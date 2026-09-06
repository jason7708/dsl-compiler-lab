#include "dsl/objects.h"
#include "dsl/math.h"
#include "dsl/visit.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

namespace dsl {
namespace {
class InvalidObject final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
std::string cppType(const Value &value) {
    return value.type == Type::Record ? "::" + value.recordName : std::string(typeName(value.type));
}

// Resolve the error effect of every operation before expanding calls. A caller
// either shares its callee's typed error or diagnoses an incompatible contract.
void resolveContracts(Module &module) {
    std::vector<int> status(module.functions.size());
    auto visit = [&](auto &&self, FunctionId id, unsigned depth) -> void {
        if (depth > 256)
            throw InvalidObject(module.functions[id].location +
                                ": error: object call graph exceeds 256 levels");
        if (status[id] == 2)
            return;
        auto &function = module.functions[id];
        if (status[id] == 1)
            throw InvalidObject(function.location +
                                ": error: recursive DSL calls are not supported in object mode");
        status[id] = 1;
        for (const auto &value : function.values) {
            const auto *call = std::get_if<Call>(&value.operation);
            if (!call)
                continue;
            const auto *callee = std::get_if<DslFunction>(&call->target);
            if (!callee)
                continue;
            self(self, callee->id, depth + 1);
            const auto &error = module.functions[callee->id].errorRecord;
            if (error.empty())
                continue;
            if (!function.errorRecord.empty() && function.errorRecord != error)
                throw InvalidObject(
                    call->location +
                    ": error: composed operations must share one error record type");
            function.errorRecord = error;
        }
        status[id] = 2;
    };
    for (FunctionId id = 0; id < module.functions.size(); ++id)
        visit(visit, id, 0);
}

class ObjectLowering {
  public:
    explicit ObjectLowering(const Module &module) : module_(module) {}
    Function lower(FunctionId id) {
        const auto &source = module_.functions[id];
        output_ = source;
        output_.values.clear();
        output_.body = {};
        std::vector<ValueId> mapping(source.values.size());
        for (const auto [index, value] : source.values | std::views::enumerate)
            if (std::holds_alternative<Parameter>(value.operation)) {
                mapping[index] = add(value, output_.body);
                if (std::get<Parameter>(value.operation).state)
                    output_.stateParameter = mapping[index];
            }
        cloneRegion(source, source.body, output_.body, mapping, false, "", 0);
        std::unordered_set<std::string> names;
        for (auto &value : output_.values)
            if (auto *read = std::get_if<ContextRead>(&value.operation)) {
                auto stem = read->field;
                if (stem == output_.name + "_context")
                    stem = "input_" + stem;
                auto name = stem;
                std::size_t suffix = 1;
                while (!names.insert(name).second)
                    name = std::format("{}_{}", stem, suffix++);
                read->field = name;
            }
        return std::move(output_);
    }

  private:
    ValueId add(Value value, Region &region) {
        if (output_.values.size() >= 100000)
            throw InvalidObject(output_.location +
                                ": error: object expansion exceeds the 100000-value limit");
        const auto id = output_.values.size();
        output_.values.push_back(std::move(value));
        region.instructions.push_back(id);
        return id;
    }
    bool eventInput(ValueId id) const {
        if (unstable_.contains(id))
            return false;
        return std::visit(Overloaded{
                              [](const Parameter &op) { return !op.state; },
                              [](const Constant &) { return true; },
                              [](const IntegerConstant &) { return true; },
                              [](const BooleanConstant &) { return true; },
                              [&](const Member &op) { return eventInput(op.base); },
                              [&](const Reference &op) { return eventInput(op.target); },
                              [](const auto &) { return false; },
                          },
                          output_.values[id].operation);
    }
    bool canExit(const Function &function, const Region &region) const {
        for (const auto id : region.instructions) {
            const bool exits = std::visit(
                Overloaded{
                    [](const Return &) { return true; },
                    [&](const If &op) {
                        return canExit(function, op.whenTrue) || canExit(function, op.whenFalse);
                    },
                    [&](const Scope &op) { return canExit(function, op.body); },
                    [&](const Select &op) {
                        return canExit(function, op.whenTrue) || canExit(function, op.whenFalse);
                    },
                    [&](const Call &op) {
                        const auto *callee = std::get_if<DslFunction>(&op.target);
                        return callee && !module_.functions[callee->id].errorRecord.empty();
                    },
                    [](const auto &) { return false; },
                },
                function.values[id].operation);
            if (exits)
                return true;
        }
        return false;
    }
    void cloneRegion(const Function &source, const Region &input, Region &region,
                     std::vector<ValueId> &mapping, bool conditional, const std::string &prefix,
                     unsigned depth) {
        if (depth > 256)
            throw InvalidObject(source.location +
                                ": error: object call expansion exceeds 256 levels");
        std::unordered_set<ValueId> mutated;
        for (const auto &value : source.values) {
            if (const auto *op = std::get_if<Store>(&value.operation))
                mutated.insert(op->target);
            if (const auto *op = std::get_if<FieldStore>(&value.operation))
                mutated.insert(op->base);
        }
        for (const auto id : input.instructions) {
            const auto &value = source.values[id];
            if (std::holds_alternative<Parameter>(value.operation))
                continue;
            if (const auto *call = std::get_if<Call>(&value.operation)) {
                std::vector<ValueId> arguments;
                for (const auto arg : call->arguments)
                    arguments.push_back(mapping[arg]);
                if (const auto *callee = std::get_if<DslFunction>(&call->target)) {
                    const auto &child = module_.functions[callee->id];
                    Region body;
                    std::vector<ValueId> childMapping(child.values.size());
                    std::optional<ValueId> state, callerState;
                    std::size_t argument = 0;
                    for (const auto [index, parameter] : child.values | std::views::enumerate) {
                        const auto *param = std::get_if<Parameter>(&parameter.operation);
                        if (!param)
                            continue;
                        const auto actual = arguments.at(argument++);
                        if (param->state) {
                            callerState = actual;
                            state = add({.type = Type::Record,
                                         .operation = StateCopy{actual},
                                         .recordName = child.stateRecord},
                                        body);
                            childMapping[index] = *state;
                        } else
                            childMapping[index] = actual;
                    }
                    cloneRegion(child, child.body, body, childMapping, conditional,
                                std::format("{}{}_{}_", prefix, child.name, callSite_++),
                                depth + 1);
                    mapping[id] =
                        add({.type = value.type,
                             .operation = Invoke{callee->id, std::move(body), state, callerState},
                             .recordName = value.recordName},
                            region);
                    if (!child.errorRecord.empty())
                        conditional = true;
                    continue;
                }
                if (const auto *external = std::get_if<ExternalFunction>(&call->target)) {
                    const auto &declaration = module_.externals[external->id];
                    auto reject = [&](const std::string &message) {
                        throw InvalidObject(call->location + ": error: " + message + ": " +
                                            declaration.name);
                    };
                    if (!declaration.context)
                        reject("external call must be registered with --context-function in object "
                               "mode");
                    if (conditional)
                        reject("context reads cannot occur inside conditional branches or after a "
                               "possible return/error");
                    if (!std::ranges::all_of(arguments,
                                             [&](ValueId arg) { return eventInput(arg); }))
                        reject("context arguments must be immutable event fields, parameters or "
                               "literal constants");
                    mapping[id] = add(
                        {.type = value.type,
                         .operation =
                             ContextRead{external->id, std::move(arguments),
                                         std::format("{}input_{}", prefix, output_.values.size())}},
                        region);
                    continue;
                }
                mapping[id] =
                    add({.type = value.type,
                         .operation = Call{call->target, std::move(arguments), call->location},
                         .recordName = value.recordName},
                        region);
                continue;
            }
            auto cloneBranch = [&](const Region &input, bool guarded) {
                Region branch;
                auto branchMapping = mapping;
                cloneRegion(source, input, branch, branchMapping, guarded, prefix, depth);
                if (!input.instructions.empty())
                    branch.result = branchMapping[input.result];
                return branch;
            };
            auto operation = std::visit(
                Overloaded{
                    [&](const Binary &op) -> Operation {
                        return Binary{op.op, mapping[op.lhs], mapping[op.rhs]};
                    },
                    [&](const Unary &op) -> Operation { return Unary{op.op, mapping[op.operand]}; },
                    [&](const Member &op) -> Operation {
                        return Member{mapping[op.base], op.field};
                    },
                    [&](const Reference &op) -> Operation {
                        const auto target = mapping[op.target];
                        if (auto *read =
                                std::get_if<ContextRead>(&output_.values[target].operation);
                            read && !op.name.empty())
                            read->field = prefix + op.name;
                        return Reference{target, op.name};
                    },
                    [&](const RecordInit &op) -> Operation {
                        std::vector<ValueId> fields;
                        for (auto field : op.fields)
                            fields.push_back(mapping[field]);
                        return RecordInit{std::move(fields)};
                    },
                    [&](const Store &op) -> Operation {
                        return Store{mapping[op.target], mapping[op.value]};
                    },
                    [&](const FieldStore &op) -> Operation {
                        return FieldStore{mapping[op.base], op.field, mapping[op.value]};
                    },
                    [&](const Return &op) -> Operation {
                        return Return{mapping[op.value], op.failure};
                    },
                    [&](const Scope &op) -> Operation {
                        Region body; // Scope still maps enclosing variables, but new declarations
                                     // do not escape in the AST bindings.
                        cloneRegion(source, op.body, body, mapping, conditional, prefix, depth);
                        return Scope{std::move(body)};
                    },
                    [&](const If &op) -> Operation {
                        return If{mapping[op.condition], cloneBranch(op.whenTrue, true),
                                  cloneBranch(op.whenFalse, true)};
                    },
                    [&](const Select &op) -> Operation {
                        return Select{mapping[op.condition], cloneBranch(op.whenTrue, true),
                                      cloneBranch(op.whenFalse, true)};
                    },
                    [](const auto &op) -> Operation { return op; },
                },
                value.operation);
            mapping[id] = add({.type = value.type,
                               .operation = std::move(operation),
                               .recordName = value.recordName},
                              region);
            if (mutated.contains(id))
                unstable_.insert(mapping[id]);
            Region singleton{.instructions = {id}};
            if (canExit(source, singleton))
                conditional = true;
        }
    }
    const Module &module_;
    Function output_;
    std::unordered_set<ValueId> unstable_;
    std::size_t callSite_ = 0;
};

class ObjectEmitter {
  public:
    ObjectEmitter(const Module &module, FunctionId id)
        : module_(module), id_(id), function_(module.functions[id]) {
        parameterNames_.resize(function_.values.size());
        std::unordered_set<std::string> used;
        for (const auto [id, value] : function_.values | std::views::enumerate) {
            const auto *parameter = std::get_if<Parameter>(&value.operation);
            if (parameter && !parameter->state) {
                parameters_.push_back(id);
                if (!parameter->name.empty()) {
                    parameterNames_[id] = parameter->name;
                    used.insert(parameter->name);
                }
            }
            if (std::holds_alternative<ContextRead>(value.operation))
                reads_.push_back(id);
        }
        for (const auto id : parameters_)
            if (parameterNames_[id].empty()) {
                auto stem = std::format("arg_{}", id), name = stem;
                unsigned suffix = 1;
                while (!used.insert(name).second)
                    name = std::format("{}_{}", stem, suffix++);
                parameterNames_[id] = name;
            }
        recordEvent_ =
            parameters_.size() == 1 && function_.values[parameters_[0]].type == Type::Record;
    }
    std::string types() const {
        const auto &name = function_.name;
        std::string text = "struct " + name + "_context {\n";
        for (auto id : reads_)
            text +=
                "    double " + std::get<ContextRead>(function_.values[id].operation).field + ";\n";
        text += "};\n";
        if (recordEvent_)
            text += std::format("using {}_event = {};\n", name,
                                cppType(function_.values[parameters_[0]]));
        else {
            text += "struct " + name + "_event {\n";
            for (auto id : parameters_)
                text +=
                    std::format("    {} {};\n", cppType(function_.values[id]), parameterNames_[id]);
            text += "};\n";
        }
        text += function_.stateRecord.empty()
                    ? "struct " + name + "_state {};\n"
                    : std::format("using {}_state = ::{};\n", name, function_.stateRecord);
        text += function_.errorRecord.empty()
                    ? "struct " + name + "_error {};\n"
                    : std::format("using {}_error = ::{};\n", name, function_.errorRecord);
        text += std::format("using {}_result = {};\n", name,
                            function_.returnType == Type::Record
                                ? "::" + function_.returnRecord
                                : std::string(typeName(function_.returnType)));
        text += std::format("struct {0}_intent {{}};\nusing {0}_contract = "
                            "dsl_runtime::contract<{0}_context, {0}_state, {0}_event, {0}_result, "
                            "{0}_error, {0}_intent>;\nusing {0}_output = {0}_contract::output;\n\n",
                            name);
        return text;
    }
    std::string definitions() const {
        const auto &name = function_.name;
        std::string text = std::format(
            "inline {0}_context prepare_{0}_context([[maybe_unused]] const {0}_event& event) {{\n",
            name);
        std::unordered_set<ValueId> needed;
        auto mark = [&](auto &&self, ValueId id) -> void {
            if (!needed.insert(id).second)
                return;
            std::visit(Overloaded{
                           [&](const ContextRead &op) {
                               for (auto arg : op.arguments)
                                   self(self, arg);
                           },
                           [&](const Reference &op) { self(self, op.target); },
                           [&](const Member &op) { self(self, op.base); },
                           [](const auto &) {},
                       },
                       function_.values[id].operation);
        };
        for (auto id : reads_)
            mark(mark, id);
        // Values are numbered in evaluation order, including expanded helper bodies.
        for (const auto [id, value] : function_.values | std::views::enumerate)
            if (needed.contains(id))
                text += declaration(id, expression(id, true), "    ");
        text += "    return {";
        for (const auto [index, id] : reads_ | std::views::enumerate) {
            if (index)
                text += ", ";
            text += std::format(".{} = v{}",
                                std::get<ContextRead>(function_.values[id].operation).field, id);
        }
        text += "};\n}\n";
        text +=
            std::format("struct {0} {{\n    using contract_t = {0}_contract;\n    [[nodiscard]] "
                        "contract_t::response operator()(\n        [[maybe_unused]] const "
                        "{0}_context& ctx, [[maybe_unused]] const {0}_state& state,\n        "
                        "[[maybe_unused]] const {0}_event& event) const {{\n",
                        name);
        text += region(function_.body, "        ", id_, function_.stateParameter);
        text += "    }\n};\n";
        for (const auto suffix : {"context", "event", "state", "result", "error", "output"})
            text += std::format("static_assert(std::is_standard_layout_v<{0}_{1}> && "
                                "std::is_trivially_copyable_v<{0}_{1}>);\n",
                                name, suffix);
        return text + '\n';
    }

  private:
    std::string args(const std::vector<ValueId> &values) const {
        std::string text;
        for (auto id : values) {
            if (!text.empty())
                text += ", ";
            text += std::format("v{}", id);
        }
        return text;
    }
    std::string declaration(ValueId id, const std::string &expression,
                            const std::string &indent) const {
        const auto &value = function_.values[id];
        if (const auto *parameter = std::get_if<Parameter>(&value.operation);
            parameter && !parameter->state && recordEvent_)
            return std::format("{}[[maybe_unused]] const auto& v{} = {};\n", indent, id,
                               expression);
        const bool mutableValue = std::holds_alternative<Reference>(value.operation) ||
                                  std::holds_alternative<StateCopy>(value.operation) ||
                                  (std::holds_alternative<Parameter>(value.operation) &&
                                   std::get<Parameter>(value.operation).state);
        return std::format("{}[[maybe_unused]] {}{} v{} = {};\n", indent,
                           mutableValue ? "" : "const ", cppType(value), id, expression);
    }
    std::string expression(ValueId id, bool binding = false) const {
        const auto &value = function_.values[id];
        return std::visit(
            Overloaded{
                [&](const Parameter &op) {
                    return op.state       ? std::string("state")
                           : recordEvent_ ? std::string("event")
                                          : "event." + parameterNames_[id];
                },
                [](const Constant &op) { return doubleLiteral(op.value); },
                [](const IntegerConstant &op) { return std::format("{}", op.value); },
                [](const BooleanConstant &op) { return std::format("{}", op.value); },
                [](const Member &op) { return std::format("v{}.{}", op.base, op.field); },
                [](const Reference &op) { return std::format("v{}", op.target); },
                [](const StateCopy &op) { return std::format("v{}", op.source); },
                [](const Binary &op) {
                    return std::format("(v{} {} v{})", op.lhs, opSymbol(op.op), op.rhs);
                },
                [](const Unary &op) { return std::format("({}v{})", opSymbol(op.op), op.operand); },
                [&](const RecordInit &op) { return cppType(value) + "{" + args(op.fields) + "}"; },
                [&](const ContextRead &op) {
                    return binding ? std::format("::{}({})", module_.externals[op.external].name,
                                                 args(op.arguments))
                                   : "ctx." + op.field;
                },
                [&](const Call &op) {
                    return std::format("{}({})",
                                       mathBuiltin(std::get<MathFunction>(op.target)).cppName,
                                       args(op.arguments));
                },
                [](const auto &) -> std::string {
                    throw std::logic_error("statement used as an expression");
                },
            },
            value.operation);
    }
    std::string region(const Region &body, const std::string &indent, FunctionId current,
                       std::optional<ValueId> state) const {
        std::string text;
        for (auto id : body.instructions) {
            const auto &value = function_.values[id];
            if (const auto *op = std::get_if<Return>(&value.operation)) {
                text += op->failure
                            ? std::format("{}return std::unexpected(v{});\n", indent, op->value)
                            : std::format("{}return {}_output{{.result = v{}, .intent = {{}}, "
                                          ".new_state = {}}};\n",
                                          indent, module_.functions[current].name, op->value,
                                          state ? std::format("v{}", *state) : "{}");
            } else if (const auto *op = std::get_if<Store>(&value.operation)) {
                text += std::format("{}v{} = v{};\n", indent, op->target, op->value);
            } else if (const auto *op = std::get_if<FieldStore>(&value.operation)) {
                text += std::format("{}v{}.{} = v{};\n", indent, op->base, op->field, op->value);
            } else if (const auto *op = std::get_if<If>(&value.operation)) {
                text +=
                    std::format("{}if (v{}) {{\n{}{}}} else {{\n{}{}}}\n", indent, op->condition,
                                region(op->whenTrue, indent + "    ", current, state), indent,
                                region(op->whenFalse, indent + "    ", current, state), indent);
            } else if (const auto *op = std::get_if<Scope>(&value.operation)) {
                text += indent + "{\n" + region(op->body, indent + "    ", current, state) +
                        indent + "}\n";
            } else if (const auto *op = std::get_if<Select>(&value.operation)) {
                text += std::format("{}[[maybe_unused]] {} v{}{{}};\n{}if (v{}) {{\n{}{}    v{} = "
                                    "v{};\n{}}} else {{\n{}{}    v{} = v{};\n{}}}\n",
                                    indent, cppType(value), id, indent, op->condition,
                                    region(op->whenTrue, indent + "    ", current, state), indent,
                                    id, op->whenTrue.result, indent,
                                    region(op->whenFalse, indent + "    ", current, state), indent,
                                    id, op->whenFalse.result, indent);
            } else if (const auto *op = std::get_if<Invoke>(&value.operation)) {
                const auto &child = module_.functions[op->function];
                text += std::format(
                    "{}auto call_{} = [&]() -> {}_contract::response {{\n{}{}}}();\n", indent, id,
                    child.name, region(op->body, indent + "    ", op->function, op->state), indent);
                if (!child.errorRecord.empty())
                    text +=
                        std::format("{}if (!call_{}) return std::unexpected(call_{}.error());\n",
                                    indent, id, id);
                if (op->callerState)
                    text +=
                        std::format("{}v{} = call_{}->new_state;\n", indent, *op->callerState, id);
                text += declaration(id, std::format("call_{}->result", id), indent);
            } else
                text += declaration(id, expression(id), indent);
        }
        return text;
    }
    const Module &module_;
    FunctionId id_;
    const Function &function_;
    std::vector<ValueId> parameters_, reads_;
    std::vector<std::string> parameterNames_;
    bool recordEvent_ = false;
};
} // namespace

std::expected<Module, std::string> lowerObjects(Module module) {
    try {
        resolveContracts(module);
        std::unordered_set<std::string> names(module.importedNames.begin(),
                                              module.importedNames.end());
        for (const auto name : {"std", "dsl_runtime", "dsl_math", "contract_t"})
            names.insert(name);
        for (const auto &function : module.functions) {
            std::vector<std::string> generated{function.name,
                                               "prepare_" + function.name + "_context"};
            for (const auto suffix :
                 {"context", "event", "state", "result", "error", "intent", "contract", "output"})
                generated.push_back(function.name + "_" + suffix);
            for (const auto &name : generated)
                if (!names.insert(name).second)
                    throw InvalidObject(
                        function.location +
                        ": error: generated object name conflicts with another declaration: " +
                        name);
        }
        const Module input = std::move(module);
        Module result = input;
        for (FunctionId id = 0; id < input.functions.size(); ++id)
            result.functions[id] = ObjectLowering(input).lower(id);
        return result;
    } catch (const InvalidObject &error) {
        return std::unexpected(error.what());
    }
}
std::string generateObjects(const Module &module) {
    std::string text = "// Generated function objects from typed DSL IR. C++23.\n#pragma "
                       "once\n#include <dsl_runtime/operation.h>\n#include <dsl_runtime/math.h>\n";
    for (const auto &header : module.externalHeaders)
        text += std::format("#include \"{}\"\n", header);
    for (const auto &macro : module.externalMacros)
        text += "#undef " + macro + "\n";
    text += '\n';
    for (const auto &record : module.records)
        if (!record.external) {
            text += "struct " + record.name + " {\n";
            for (const auto &field : record.fields)
                text += std::format("    {} {};\n", typeName(field.type), field.name);
            text += "};\n";
        }
    for (FunctionId id = 0; id < module.functions.size(); ++id)
        text += ObjectEmitter(module, id).types();
    for (FunctionId id = 0; id < module.functions.size(); ++id)
        text += ObjectEmitter(module, id).definitions();
    return text;
}
} // namespace dsl
