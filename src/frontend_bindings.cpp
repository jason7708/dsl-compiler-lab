#include "dsl/visit.h"
#include "frontend_ir.h"
#include "frontend_math.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

namespace dsl::source {
namespace {
class InvalidObject final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
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
                    [&](const StateView &op) -> Operation {
                        return StateView{mapping[op.base], op.fields};
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

} // namespace

std::expected<Module, std::string> planBindings(Module module) {
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
                 {"context", "event", "state", "result", "error", "contract", "output"})
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
} // namespace dsl::source
