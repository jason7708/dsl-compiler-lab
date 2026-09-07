#include "dsl/frontend.h"
#include "dsl/verify.h"
#include "frontend_ir.h"
#include "frontend_math.h"
#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
namespace dsl {
namespace {
namespace s = source;
namespace c = ir;
class Semantic {
    const s::Module &source;
    Program program;
    std::map<std::string, c::TypeId> records;
    std::map<std::uint32_t, c::ConstantId> defaults;
    const s::Function *input = nullptr;
    c::Function *output = nullptr;
    std::set<s::ValueId> mutableIds;
    std::uint32_t nextOperation = 0;
    using Env = std::vector<std::optional<c::ValueId>>;
    static std::uint32_t index(std::size_t n) {
        if (n > 1000000)
            throw std::runtime_error("IR size limit exceeded");
        return static_cast<std::uint32_t>(n);
    }
    c::TypeId type(s::Type t, const std::string &record = {}) const {
        switch (t) {
        case s::Type::Double:
            return {0};
        case s::Type::Bool:
            return {1};
        case s::Type::Int:
            return {2};
        case s::Type::Record:
            return records.at(record);
        }
        throw std::runtime_error("invalid source type");
    }
    c::TypeId type(s::ValueId id) const {
        const auto &v = input->values.at(id);
        return type(v.type, v.recordName);
    }
    c::ConstantId constant(c::TypeId t, std::uint64_t bits) {
        c::ConstantId id{index(program.computation.constants.size())};
        program.computation.constants.push_back({t, bits});
        return id;
    }
    c::ConstantId initial(s::Type t, const std::optional<s::InitialValue> &value) {
        if (!value)
            return constant(type(t), 0);
        if (auto *x = std::get_if<double>(&*value))
            return constant(type(t), std::bit_cast<std::uint64_t>(*x));
        if (auto *x = std::get_if<int>(&*value))
            return constant(type(t), static_cast<std::uint32_t>(*x));
        return constant(type(t), std::get<bool>(*value) ? 1 : 0);
    }
    c::FieldId field(c::TypeId t, const std::string &name) const {
        const auto &layout = *std::ranges::find(program.cpp.records, t, &CppRecord::type);
        auto it = std::ranges::find(layout.fields, name);
        if (it == layout.fields.end())
            throw std::runtime_error("unknown source record field: " + name);
        return {index(static_cast<std::size_t>(it - layout.fields.begin()))};
    }
    c::ValueId get(const Env &env, s::ValueId id) const {
        if (id >= env.size() || !env[id])
            throw std::runtime_error("source lowering produced an unavailable value");
        return *env[id];
    }
    c::ValueId newValue(c::TypeId t) {
        c::ValueId id{index(output->values.size())};
        output->values.push_back(t);
        return id;
    }
    c::Operation make(c::OpCode code, std::vector<c::ValueId> operands,
                      std::vector<c::TypeId> results, c::Attribute attr = {}) {
        c::Operation op{{nextOperation++}, code, std::move(operands), {},
                        std::move(attr),   {},   input->location};
        for (auto t : results)
            op.results.push_back(newValue(t));
        return op;
    }
    c::ValueId emit(c::Region &region, c::OpCode code, std::vector<c::ValueId> operands,
                    c::TypeId t, c::Attribute attr = {}) {
        auto op = make(code, std::move(operands), {t}, std::move(attr));
        auto id = op.results[0];
        region.operations.push_back(std::move(op));
        return id;
    }
    c::ValueId literal(c::Region &region, c::ConstantId id) {
        return emit(region, c::OpCode::Constant, {}, program.computation.constants[id.value].type,
                    id);
    }
    c::ValueId extract(c::Region &region, c::ValueId base, c::FieldId id) {
        const auto t = output->values[base.value];
        const auto &fields = std::get<c::RecordType>(program.computation.types[t.value]).fields;
        return emit(region, c::OpCode::Extract, {base}, fields.at(id.value).type, id);
    }
    c::OpCode operation(std::string_view name) const {
        for (const auto &entry : c::registry())
            if (entry.name == name)
                return entry.code;
        throw std::runtime_error("unregistered source OP: " + std::string(name));
    }
    std::vector<s::ValueId> liveMutables(const Env &env) const {
        std::vector<s::ValueId> result;
        for (auto id : mutableIds)
            if (env[id])
                result.push_back(id);
        return result;
    }
    std::vector<c::ValueId> mapped(const Env &env, const std::vector<s::ValueId> &ids) const {
        std::vector<c::ValueId> values;
        for (auto id : ids)
            values.push_back(get(env, id));
        return values;
    }
    std::vector<c::TypeId> types(const std::vector<c::ValueId> &values) const {
        std::vector<c::TypeId> result;
        for (auto id : values)
            result.push_back(output->values[id.value]);
        return result;
    }
    BindingArgument bindingArgument(s::ValueId id) {
        const auto &value = input->values[id];
        if (const auto *p = std::get_if<s::Parameter>(&value.operation)) {
            if (p->state)
                throw std::runtime_error("state cannot supply a host binding");
            std::size_t parameter = 0;
            for (std::size_t i = 0; i < id; ++i)
                if (std::holds_alternative<s::Parameter>(input->values[i].operation))
                    ++parameter;
            return {parameter, {}};
        }
        if (const auto *p = std::get_if<s::Reference>(&value.operation))
            return bindingArgument(p->target);
        if (const auto *p = std::get_if<s::Member>(&value.operation)) {
            auto argument = bindingArgument(p->base);
            argument.fields.push_back(field(type(p->base), p->field));
            return argument;
        }
        if (const auto *p = std::get_if<s::Constant>(&value.operation))
            return {constant(type(id), std::bit_cast<std::uint64_t>(p->value)), {}};
        if (const auto *p = std::get_if<s::IntegerConstant>(&value.operation))
            return {constant(type(id), static_cast<std::uint32_t>(p->value)), {}};
        if (const auto *p = std::get_if<s::BooleanConstant>(&value.operation))
            return {constant(type(id), p->value ? 1 : 0), {}};
        throw std::runtime_error("unsupported host binding argument");
    }
    // Assignments disappear here: each environment entry denotes its current immutable value.
    bool region(const s::Region &from, c::Region &to, Env &env, std::optional<s::ValueId> state) {
        for (auto id : from.instructions) {
            const auto &v = input->values[id];
            if (std::holds_alternative<s::Parameter>(v.operation) ||
                std::holds_alternative<s::ContextRead>(v.operation))
                continue;
            if (const auto *p = std::get_if<s::Return>(&v.operation)) {
                if (p->failure)
                    to.terminator = c::ReturnError{get(env, p->value)};
                else {
                    auto values = std::vector{get(env, p->value)};
                    if (state)
                        values.push_back(get(env, *state));
                    to.terminator = c::ReturnSuccess{std::move(values)};
                }
                return false;
            }
            if (const auto *p = std::get_if<s::Store>(&v.operation)) {
                env[p->target] = get(env, p->value);
                continue;
            }
            if (const auto *p = std::get_if<s::FieldStore>(&v.operation)) {
                env[p->base] = emit(to, c::OpCode::Insert, {get(env, p->base), get(env, p->value)},
                                    type(p->base), field(type(p->base), p->field));
                continue;
            }
            if (const auto *p = std::get_if<s::StateCopy>(&v.operation)) {
                env[id] = get(env, p->source);
                continue;
            }
            if (const auto *p = std::get_if<s::StateView>(&v.operation)) {
                std::vector<c::ValueId> values;
                for (const auto &name : p->fields)
                    values.push_back(extract(to, get(env, p->base), field(type(p->base), name)));
                env[id] = emit(to, c::OpCode::Aggregate, std::move(values), type(id));
                continue;
            }
            if (const auto *p = std::get_if<s::Invoke>(&v.operation)) {
                auto resultTypes = std::vector{type(id)};
                if (p->state)
                    resultTypes.push_back(type(*p->state));
                const auto &child = source.functions[p->function];
                const auto error = child.errorRecord.empty()
                                       ? std::nullopt
                                       : std::optional{records.at(child.errorRecord)};
                auto op = make(c::OpCode::Evaluate, {}, resultTypes, c::Evaluation{error});
                auto local = env;
                c::Region body;
                if (region(p->body, body, local, p->state))
                    throw std::runtime_error("callee lacks terminator");
                op.regions.push_back(std::move(body));
                env[id] = op.results[0];
                auto next = p->state ? std::optional{op.results[1]} : std::nullopt;
                to.operations.push_back(std::move(op));
                if (p->callerState) {
                    if (const auto *view =
                            std::get_if<s::StateView>(&input->values[*p->callerState].operation)) {
                        auto base = get(env, view->base);
                        const auto &fields =
                            std::get<c::RecordType>(
                                program.computation.types[output->values[next->value].value])
                                .fields;
                        for (std::size_t i = 0; i < view->fields.size(); ++i) {
                            auto leaf = extract(to, *next, fields[i].id);
                            base = emit(to, c::OpCode::Insert, {base, leaf}, type(view->base),
                                        field(type(view->base), view->fields[i]));
                        }
                        env[view->base] = base;
                    } else
                        env[*p->callerState] = *next;
                }
                continue;
            }
            if (std::holds_alternative<s::If>(v.operation) ||
                std::holds_alternative<s::Select>(v.operation) ||
                std::holds_alternative<s::Scope>(v.operation)) {
                const auto live = liveMutables(env);
                auto resultTypes = types(mapped(env, live));
                const auto *select = std::get_if<s::Select>(&v.operation);
                const auto *conditional = std::get_if<s::If>(&v.operation);
                if (select)
                    resultTypes.insert(resultTypes.begin(), type(id));
                auto op = make(std::holds_alternative<s::Scope>(v.operation) ? c::OpCode::Scope
                                                                             : c::OpCode::If,
                               select        ? std::vector{get(env, select->condition)}
                               : conditional ? std::vector{get(env, conditional->condition)}
                                             : std::vector<c::ValueId>{},
                               resultTypes);
                std::vector<const s::Region *> branches;
                if (select)
                    branches = {&select->whenTrue, &select->whenFalse};
                else if (conditional)
                    branches = {&conditional->whenTrue, &conditional->whenFalse};
                else
                    branches = {&std::get<s::Scope>(v.operation).body};
                bool continues = false;
                for (const auto *branch : branches) {
                    auto local = env;
                    c::Region body;
                    if (region(*branch, body, local, state)) {
                        auto yielded = mapped(local, live);
                        if (select)
                            yielded.insert(yielded.begin(), get(local, branch->result));
                        body.terminator = c::Yield{std::move(yielded)};
                        continues = true;
                    }
                    op.regions.push_back(std::move(body));
                }
                for (std::size_t i = 0; i < live.size(); ++i)
                    env[live[i]] = op.results[i + (select ? 1 : 0)];
                if (select)
                    env[id] = op.results[0];
                to.operations.push_back(std::move(op));
                if (!continues) {
                    to.terminator = c::Unreachable{};
                    return false;
                }
                continue;
            }
            if (const auto *p = std::get_if<s::Constant>(&v.operation))
                env[id] = literal(to, constant(type(id), std::bit_cast<std::uint64_t>(p->value)));
            else if (const auto *p = std::get_if<s::IntegerConstant>(&v.operation))
                env[id] = literal(to, constant(type(id), static_cast<std::uint32_t>(p->value)));
            else if (const auto *p = std::get_if<s::BooleanConstant>(&v.operation))
                env[id] = literal(to, constant(type(id), p->value ? 1 : 0));
            else if (const auto *p = std::get_if<s::Reference>(&v.operation))
                env[id] = emit(to, c::OpCode::Identity, {get(env, p->target)}, type(id));
            else if (const auto *p = std::get_if<s::Member>(&v.operation))
                env[id] = extract(to, get(env, p->base), field(type(p->base), p->field));
            else if (const auto *p = std::get_if<s::Binary>(&v.operation))
                env[id] = emit(to, operation(s::opName(p->op)),
                               {get(env, p->lhs), get(env, p->rhs)}, type(id));
            else if (const auto *p = std::get_if<s::Unary>(&v.operation))
                env[id] = emit(to,
                               p->op == s::UnaryOp::Plus     ? c::OpCode::Positive
                               : p->op == s::UnaryOp::Negate ? c::OpCode::Negate
                                                             : c::OpCode::Not,
                               {get(env, p->operand)}, type(id));
            else if (const auto *p = std::get_if<s::RecordInit>(&v.operation)) {
                auto args = mapped(env, p->fields);
                const auto &initial = std::get<std::vector<c::ConstantId>>(
                    program.computation.constants[defaults.at(type(id).value).value].payload);
                for (std::size_t i = args.size(); i < initial.size(); ++i)
                    args.push_back(literal(to, initial[i]));
                env[id] = emit(to, c::OpCode::Aggregate, std::move(args), type(id));
            } else if (const auto *p = std::get_if<s::Call>(&v.operation)) {
                c::OpCode code;
                c::Attribute attr;
                if (const auto *math = std::get_if<s::MathFunction>(&p->target))
                    code = operation(s::mathBuiltin(*math).name);
                else if (const auto *function = std::get_if<s::DslFunction>(&p->target)) {
                    code = c::OpCode::Call;
                    attr = c::FunctionId{index(function->id)};
                } else {
                    code = c::OpCode::ExternalCall;
                    attr = c::ExternalId{index(std::get<s::ExternalFunction>(p->target).id)};
                }
                env[id] = emit(to, code, mapped(env, p->arguments), type(id), attr);
            } else
                throw std::runtime_error("unsupported source operation at semantic boundary");
        }
        return true;
    }

  public:
    explicit Semantic(const s::Module &module) : source(module) {}
    Program run() {
        auto &m = program.computation;
        m.types = {c::FloatType{c::FloatFormat::IEEE754Binary64}, c::BoolType{},
                   c::IntegerType{32, c::Signedness::Signed}};
        program.cpp.headers = source.externalHeaders;
        program.cpp.macros = source.externalMacros;
        for (const auto &record : source.records) {
            c::TypeId id{index(m.types.size())};
            records.emplace(record.name, id);
            c::RecordType t;
            CppRecord cpp{id, record.name, {}, record.external, {0}};
            std::vector<c::ConstantId> fields;
            for (const auto &leaf : record.fields) {
                t.fields.push_back({c::FieldId{index(t.fields.size())}, type(leaf.type)});
                cpp.fields.push_back(leaf.name);
                fields.push_back(initial(leaf.type, leaf.initial));
            }
            m.types.push_back(std::move(t));
            cpp.initial = c::ConstantId{index(m.constants.size())};
            defaults[id.value] = cpp.initial;
            m.constants.push_back({id, std::move(fields)});
            program.cpp.records.push_back(std::move(cpp));
        }
        for (const auto &external : source.externals) {
            c::External e{external.name, {{}, {type(s::Type::Double)}, std::nullopt}};
            for (auto t : external.parameters)
                e.signature.parameters.push_back(type(t));
            m.externals.push_back(std::move(e));
            program.cpp.externalSymbols.push_back(external.name);
        }
        m.functions.resize(source.functions.size());
        program.bindings.functions.resize(source.functions.size());
        program.units.style =
            source.objectMode ? UnitEnvelope::Style::Objects : UnitEnvelope::Style::Scalar;
        for (std::size_t i = 0; i < source.functions.size(); ++i) {
            input = &source.functions[i];
            output = &m.functions[i];
            nextOperation = 0;
            mutableIds.clear();
            output->debugName = input->name;
            Export exported{
                c::FunctionId{index(i)}, input->name, {}, {}, std::nullopt, std::nullopt};
            Env env(input->values.size());
            std::set<std::string> eventNames;
            for (const auto &v : input->values)
                if (const auto *p = std::get_if<s::Parameter>(&v.operation);
                    p && !p->state && !p->name.empty())
                    eventNames.insert(p->name);
            for (std::size_t id = 0; id < input->values.size(); ++id) {
                const auto *p = std::get_if<s::Parameter>(&input->values[id].operation);
                if (!p)
                    continue;
                auto parameter = output->parameters.size();
                auto t = type(id);
                env[id] = newValue(t);
                output->parameters.push_back(*env[id]);
                output->signature.parameters.push_back(t);
                if (p->state) {
                    exported.stateParameter = parameter;
                    exported.initialState = defaults.at(t.value);
                } else {
                    exported.eventParameters.push_back(parameter);
                    auto name = p->name;
                    if (name.empty()) {
                        auto stem = std::format("arg_{}", id);
                        name = stem;
                        unsigned suffix = 1;
                        while (!eventNames.insert(name).second)
                            name = stem + "_" + std::to_string(suffix++);
                    }
                    exported.eventFields.push_back(name);
                }
            }
            for (std::size_t id = 0; id < input->values.size(); ++id) {
                const auto &op = input->values[id].operation;
                if (const auto *p = std::get_if<s::ContextRead>(&op)) {
                    ContextBinding binding{
                        output->parameters.size(), p->field, c::ExternalId{index(p->external)}, {}};
                    for (auto arg : p->arguments)
                        binding.arguments.push_back(bindingArgument(arg));
                    env[id] = newValue(type(id));
                    output->parameters.push_back(*env[id]);
                    output->signature.parameters.push_back(type(id));
                    program.bindings.functions[i].push_back(std::move(binding));
                }
                if (const auto *p = std::get_if<s::Store>(&op))
                    mutableIds.insert(p->target);
                if (const auto *p = std::get_if<s::FieldStore>(&op))
                    mutableIds.insert(p->base);
                if (const auto *p = std::get_if<s::Invoke>(&op); p && p->callerState) {
                    const auto *view =
                        std::get_if<s::StateView>(&input->values[*p->callerState].operation);
                    mutableIds.insert(view ? view->base : *p->callerState);
                }
            }
            output->signature.results = {type(input->returnType, input->returnRecord)};
            if (input->stateParameter)
                output->signature.results.push_back(type(*input->stateParameter));
            if (!input->errorRecord.empty())
                output->signature.error = records.at(input->errorRecord);
            const bool continues = region(input->body, output->body, env, input->stateParameter);
            if (continues) {
                if (source.objectMode)
                    throw std::runtime_error("source computation lacks a return");
                output->body.terminator = c::ReturnSuccess{{get(env, input->body.result)}};
            }
            if (source.objectMode || i == source.entry)
                program.units.exports.push_back(std::move(exported));
        }
        return std::move(program);
    }
};
} // namespace
std::expected<Program, std::string> compile(std::string_view text, std::string_view filename,
                                            const CompileOptions &options) {
    auto parsed = source::parse(text, filename, options);
    if (!parsed)
        return std::unexpected("DSL frontend rejected input");
    if (options.objects) {
        auto planned = source::planBindings(std::move(*parsed));
        if (!planned)
            return std::unexpected(planned.error());
        parsed = std::move(*planned);
    }
    try {
        auto result = Semantic(*parsed).run();
        if (auto checked = ir::verify(result.computation); !checked)
            return std::unexpected(checked.error());
        return result;
    } catch (const std::exception &error) {
        return std::unexpected(std::string("semantic lowering: ") + error.what());
    }
}
} // namespace dsl
