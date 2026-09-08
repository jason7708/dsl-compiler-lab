#include "dsl/codegen.h"
#include "dsl/verify.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <set>
#include <stdexcept>
namespace dsl::cpp {
namespace {
namespace c = ir;
struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};
class Emitter {
    const Program &p;
    const c::Module &m;
    const c::Function *f = nullptr;
    std::string moduleNamespace;
    static void require(bool ok, std::string_view why) {
        if (!ok)
            throw Invalid("C++ backend: " + std::string(why));
    }
    const CppRecord &record(c::TypeId t) const {
        auto it = std::ranges::find(p.cpp.records, t, &CppRecord::type);
        require(it != p.cpp.records.end(), "missing C++ record linkage");
        return *it;
    }
    std::string type(c::TypeId t) const {
        require(t.value < m.types.size(), "unknown linkage type");
        const auto &value = m.types[t.value];
        if (std::holds_alternative<c::BoolType>(value))
            return "bool";
        if (const auto *i = std::get_if<c::IntegerType>(&value)) {
            require(i->width == 32, "only 32-bit integers are implemented by this backend");
            return i->signedness == c::Signedness::Signed ? "int" : "unsigned int";
        }
        if (const auto *fp = std::get_if<c::FloatType>(&value)) {
            require(fp->format == c::FloatFormat::IEEE754Binary64,
                    "only IEEE-754 binary64 is implemented by this backend");
            return "double";
        }
        return "::" + record(t).name;
    }
    std::string literal(c::ConstantId id) const {
        require(id.value < m.constants.size(), "unknown binding constant");
        const auto &value = m.constants[id.value];
        if (const auto *bits = std::get_if<std::uint64_t>(&value.payload)) {
            const auto &t = m.types[value.type.value];
            if (std::holds_alternative<c::BoolType>(t))
                return *bits ? "true" : "false";
            if (const auto *integer = std::get_if<c::IntegerType>(&t)) {
                if (integer->signedness == c::Signedness::Unsigned)
                    return std::format("{}u", *bits);
                return std::format("{}",
                                   std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(*bits)));
            }
            const auto number = std::bit_cast<double>(*bits);
            if (!std::isfinite(number))
                return std::format("std::bit_cast<double>(std::uint64_t{{{}}})", *bits);
            return std::format("{}0x{:a}", std::signbit(number) ? "-" : "", std::abs(number));
        }
        std::string text = type(value.type) + "{";
        for (auto element : std::get<std::vector<c::ConstantId>>(value.payload)) {
            if (text.back() != '{')
                text += ", ";
            text += literal(element);
        }
        return text + "}";
    }
    static bool identifier(std::string_view text) {
        auto letter = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        };
        if (text.empty() || !letter(text[0]))
            return false;
        static constexpr std::string_view keywords =
            " alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t "
            "char16_t char32_t class compl concept const consteval constexpr constinit const_cast "
            "continue co_await co_return co_yield decltype default delete do double dynamic_cast "
            "else enum explicit export extern false float for friend goto if inline int long "
            "mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected "
            "public register reinterpret_cast requires return short signed sizeof static "
            "static_assert static_cast struct switch template this thread_local throw true try "
            "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor "
            "xor_eq ";
        if (keywords.find(" " + std::string(text) + " ") != keywords.npos)
            return false;
        return std::ranges::all_of(text,
                                   [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
    }
    static bool qualified(std::string_view text) {
        while (true) {
            auto pos = text.find("::");
            if (!identifier(text.substr(0, pos)))
                return false;
            if (pos == text.npos)
                return true;
            text.remove_prefix(pos + 2);
        }
    }
    std::string field(c::TypeId t, c::FieldId id) const {
        const auto &fields = std::get<c::RecordType>(m.types[t.value]).fields;
        const auto it = std::ranges::find(fields, id, &c::Field::id);
        require(it != fields.end(), "unknown binding field");
        return record(t).fields.at(static_cast<std::size_t>(it - fields.begin()));
    }
    c::TypeId fieldType(c::TypeId t, c::FieldId id) const {
        require(t.value < m.types.size(), "unknown binding type");
        const auto *r = std::get_if<c::RecordType>(&m.types[t.value]);
        require(r != nullptr, "binding field requires record");
        const auto it = std::ranges::find(r->fields, id, &c::Field::id);
        require(it != r->fields.end(), "unknown binding field");
        return it->type;
    }
    c::TypeId bindingType(const Export &e, const BindingArgument &a) const {
        const auto &fn = m.functions[e.function.value];
        c::TypeId t{0};
        if (const auto *param = std::get_if<std::size_t>(&a.source)) {
            require(std::ranges::find(e.eventParameters, *param) != e.eventParameters.end(),
                    "provider argument must come from event inputs");
            t = fn.signature.parameters.at(*param);
        } else {
            auto id = std::get<c::ConstantId>(a.source);
            require(id.value < m.constants.size(), "unknown binding constant");
            t = m.constants[id.value].type;
        }
        for (auto id : a.fields)
            t = fieldType(t, id);
        return t;
    }
    bool grouped(const Export &e) const {
        return std::ranges::any_of(p.units.groups, [&](const UnitGroup &g) {
            return std::ranges::find(g.entries, e.function) != g.entries.end();
        });
    }
    const Export &entry(c::FunctionId id) const {
        auto it = std::ranges::find(p.units.exports, id, &Export::function);
        require(it != p.units.exports.end(), "unit entry is not exported");
        return *it;
    }
    bool directEvent(const Export &e) const {
        return grouped(e) || (e.eventParameters.size() == 1 &&
                              std::holds_alternative<c::RecordType>(
                                  m.types[m.functions[e.function.value]
                                              .signature.parameters[e.eventParameters[0]]
                                              .value]));
    }
    std::string eventArgument(const Export &e, std::size_t parameter) const {
        auto it = std::ranges::find(e.eventParameters, parameter);
        require(it != e.eventParameters.end(), "missing event parameter");
        return directEvent(e) ? "event" : "event." + e.eventFields[it - e.eventParameters.begin()];
    }
    std::string bindingArgument(const Export &e, const BindingArgument &a) const {
        const auto &fn = m.functions[e.function.value];
        std::string text;
        c::TypeId t{0};
        if (const auto *param = std::get_if<std::size_t>(&a.source)) {
            text = eventArgument(e, *param);
            t = fn.signature.parameters[*param];
        } else {
            auto id = std::get<c::ConstantId>(a.source);
            text = literal(id);
            t = m.constants[id.value].type;
        }
        for (auto id : a.fields) {
            text += "." + field(t, id);
            t = fieldType(t, id);
        }
        return text;
    }
    void integration() const {
        require(!p.units.exports.empty(), "at least one C++ export is required");
        require(p.units.style == UnitEnvelope::Style::Scalar ||
                    p.units.style == UnitEnvelope::Style::Objects,
                "unknown unit envelope style");
        require(p.cpp.externalSymbols.size() == m.externals.size(),
                "external symbol mapping mismatch");
        require(p.bindings.functions.size() == m.functions.size(),
                "host binding function mapping mismatch");
        for (const auto &name : p.cpp.externalSymbols)
            require(qualified(name), "invalid external symbol");
        for (const auto &macro : p.cpp.macros)
            require(identifier(macro), "invalid macro name");
        for (const auto &header : p.cpp.headers)
            require(!header.empty() && header.find_first_of("\"\n\r\\") == std::string::npos,
                    "invalid header path");
        std::set<std::uint32_t> linkedTypes;
        std::set<std::string> names{"dsl_backend", "std", "dsl_runtime", "dsl_math"};
        for (const auto &symbol : p.cpp.externalSymbols) {
            const auto root = symbol.substr(0, symbol.find("::"));
            require(root != "dsl_backend", "external symbol conflicts with backend namespace");
            names.insert(root);
        }
        for (const auto &r : p.cpp.records) {
            require(r.type.value < m.types.size() &&
                        std::holds_alternative<c::RecordType>(m.types[r.type.value]),
                    "record linkage type mismatch");
            require(linkedTypes.insert(r.type.value).second, "duplicate record linkage");
            require((r.external ? qualified(r.name) : identifier(r.name)) &&
                        names.insert(r.name).second,
                    "record name conflict");
            require(r.fields.size() == std::get<c::RecordType>(m.types[r.type.value]).fields.size(),
                    "record linkage field mismatch");
            for (const auto &field : std::get<c::RecordType>(m.types[r.type.value]).fields)
                require(!std::holds_alternative<c::RecordType>(m.types[field.type.value]),
                        "only flat records are implemented by this backend");
            std::set<std::string> fields;
            for (const auto &field : r.fields)
                require(identifier(field) && fields.insert(field).second,
                        "invalid or duplicate C++ field name");
            require(r.initial.value < m.constants.size() &&
                        m.constants[r.initial.value].type == r.type,
                    "record initializer type mismatch");
        }
        for (std::uint32_t i = 0; i < m.types.size(); ++i)
            (void)type(c::TypeId{i});
        for (const auto &external : m.externals)
            require(external.signature.results.size() == 1 && !external.signature.error,
                    "external ABI supports one infallible result");
        std::set<std::uint32_t> exported;
        for (const auto &e : p.units.exports) {
            require(e.function.value < m.functions.size() &&
                        exported.insert(e.function.value).second,
                    "invalid or duplicate export");
            require(identifier(e.name) && names.insert(e.name).second, "export name conflict");
            const auto &fn = m.functions[e.function.value];
            require(e.eventParameters.size() == e.eventFields.size(), "event mapping mismatch");
            std::set<std::size_t> inputs;
            std::set<std::string> fields;
            for (std::size_t i = 0; i < e.eventParameters.size(); ++i) {
                require(e.eventParameters[i] < fn.signature.parameters.size() &&
                            inputs.insert(e.eventParameters[i]).second,
                        "invalid or duplicate event parameter");
                require(identifier(e.eventFields[i]) && fields.insert(e.eventFields[i]).second,
                        "invalid event field");
            }
            if (e.stateParameter) {
                require(*e.stateParameter < fn.signature.parameters.size() &&
                            inputs.insert(*e.stateParameter).second,
                        "invalid state parameter");
                auto t = fn.signature.parameters[*e.stateParameter];
                require(fn.signature.results.size() == 2 && fn.signature.results[1] == t,
                        "state transition signature mismatch");
                require(e.initialState && *e.initialState == record(t).initial,
                        "initial state mapping mismatch");
            } else
                require(fn.signature.results.size() == 1 && !e.initialState,
                        "output envelope signature mismatch");
            if (p.units.style == UnitEnvelope::Style::Objects) {
                for (const auto *suffix :
                     {"context", "event", "state", "error", "result", "contract", "output"}) {
                    auto name = e.name + "_" + suffix;
                    if (name == e.name + "_state" && e.stateParameter &&
                        record(fn.signature.parameters[*e.stateParameter]).name == name)
                        continue;
                    require(names.insert(name).second, "generated envelope name conflict");
                }
                require(names.insert("prepare_" + e.name + "_context").second,
                        "generated binder name conflict");
            }
            fields.clear();
            for (const auto &b : p.bindings.functions[e.function.value]) {
                require(b.parameter < fn.signature.parameters.size() &&
                            inputs.insert(b.parameter).second,
                        "invalid context parameter");
                require(identifier(b.field) && fields.insert(b.field).second,
                        "invalid context field");
                require(b.provider.value < m.externals.size(), "unknown provider");
                const auto &signature = m.externals[b.provider.value].signature;
                require(signature.parameters.size() == b.arguments.size() &&
                            signature.results[0] == fn.signature.parameters[b.parameter],
                        "provider signature mismatch");
                for (std::size_t i = 0; i < b.arguments.size(); ++i)
                    require(bindingType(e, b.arguments[i]) == signature.parameters[i],
                            "provider argument type mismatch");
            }
            require(inputs.size() == fn.signature.parameters.size(), "unmapped computation input");
            if (p.units.style == UnitEnvelope::Style::Scalar)
                require(!e.stateParameter && !fn.signature.error &&
                            p.bindings.functions[e.function.value].empty(),
                        "scalar export cannot have state, errors or context providers");
        }
        std::set<std::uint32_t> groupedEntries;
        for (const auto &g : p.units.groups) {
            require(p.units.style == UnitEnvelope::Style::Objects,
                    "unit groups require object mode");
            require(identifier(g.name) && names.insert(g.name).second, "unit group name conflict");
            require(names.insert("prepare_" + g.name + "_context").second,
                    "unit group binder name conflict");
            require(g.entries.size() > 1, "unit group requires multiple entries");
            std::optional<c::TypeId> state;
            std::optional<c::ConstantId> initial;
            std::set<std::string> events;
            for (auto id : g.entries) {
                const auto &e = entry(id);
                require(groupedEntries.insert(id.value).second, "duplicate unit group entry");
                require(e.stateParameter.has_value(), "unit group entry requires state");
                require(e.eventParameters.size() == 1, "unit group entry requires one event");
                const auto &fn = m.functions[id.value];
                auto t = fn.signature.parameters[*e.stateParameter];
                require(!state || (*state == t && initial == e.initialState),
                        "unit group state mismatch");
                state = t;
                initial = e.initialState;
                require(events.insert(type(fn.signature.parameters[e.eventParameters[0]])).second,
                        "unit group requires distinct event types");
            }
        }
    }
    std::string tuple(const std::vector<c::TypeId> &types) const {
        std::string result = "std::tuple<";
        for (auto t : types) {
            if (result.back() != '<')
                result += ", ";
            result += type(t);
        }
        return result + ">";
    }
    std::string response(const c::Signature &sig) const {
        const auto success = tuple(sig.results);
        return sig.error ? "std::expected<" + success + ", " + type(*sig.error) + ">" : success;
    }
    std::string args(const std::vector<c::ValueId> &ids) const {
        std::string result;
        for (auto id : ids) {
            if (!result.empty())
                result += ", ";
            result += std::format("v{}", id.value);
        }
        return result;
    }
    std::string declaration(c::ValueId id, const std::string &expression,
                            const std::string &indent) const {
        return std::format("{}[[maybe_unused]] const {} v{} = {};\n", indent,
                           type(f->values[id.value]), id.value, expression);
    }
    std::string symbol(c::OpCode op) const {
        switch (op) {
        case c::OpCode::Add:
            return "+";
        case c::OpCode::Subtract:
            return "-";
        case c::OpCode::Multiply:
            return "*";
        case c::OpCode::Divide:
            return "/";
        case c::OpCode::Less:
            return "<";
        case c::OpCode::LessEqual:
            return "<=";
        case c::OpCode::Greater:
            return ">";
        case c::OpCode::GreaterEqual:
            return ">=";
        case c::OpCode::Equal:
            return "==";
        case c::OpCode::NotEqual:
            return "!=";
        case c::OpCode::Positive:
            return "+";
        case c::OpCode::Negate:
            return "-";
        case c::OpCode::Not:
            return "!";
        default:
            return {};
        }
    }
    // Runtime linkage belongs to this backend, not to the semantic OP registry.
    std::string math(c::OpCode op) const {
        switch (op) {
        case c::OpCode::Abs:
            return "dsl_math::abs";
        case c::OpCode::Sqrt:
            return "dsl_math::sqrt";
        case c::OpCode::Pow:
            return "dsl_math::pow";
        case c::OpCode::Exp:
            return "dsl_math::exp";
        case c::OpCode::Log:
            return "dsl_math::log";
        case c::OpCode::Sin:
            return "dsl_math::sin";
        case c::OpCode::Cos:
            return "dsl_math::cos";
        case c::OpCode::Tan:
            return "dsl_math::tan";
        case c::OpCode::Min:
            return "dsl_math::min";
        case c::OpCode::Max:
            return "dsl_math::max";
        case c::OpCode::Floor:
            return "dsl_math::floor";
        case c::OpCode::Ceil:
            return "dsl_math::ceil";
        case c::OpCode::Round:
            return "dsl_math::round";
        default:
            return {};
        }
    }
    std::string region(const c::Region &r, const c::Signature &boundary,
                       const std::vector<c::ValueId> &yields, const std::string &indent) const {
        std::string text;
        for (const auto &op : r.operations) {
            const auto *definition = c::lookup(op.code, c::registry());
            require(definition, "unknown backend OP");
            if (op.code == c::OpCode::If || op.code == c::OpCode::Scope) {
                for (auto id : op.results)
                    text += std::format("{}[[maybe_unused]] {} v{}{{}};\n", indent,
                                        type(f->values[id.value]), id.value);
                if (op.code == c::OpCode::If)
                    text += std::format("{}if (v{}) {{\n", indent, op.operands[0].value);
                else
                    text += indent + "{\n";
                text += region(op.regions[0], boundary, op.results, indent + "    ") + indent + "}";
                if (op.code == c::OpCode::If)
                    text += " else {\n" +
                            region(op.regions[1], boundary, op.results, indent + "    ") + indent +
                            "}";
                text += '\n';
                continue;
            }
            if (op.code == c::OpCode::Evaluate || op.code == c::OpCode::Call) {
                c::Signature signature;
                if (op.code == c::OpCode::Call)
                    signature = m.functions[std::get<c::FunctionId>(op.attribute).value].signature;
                else {
                    for (auto id : op.results)
                        signature.results.push_back(f->values[id.value]);
                    signature.error = std::get<c::Evaluation>(op.attribute).error;
                }
                auto call =
                    op.code == c::OpCode::Call
                        ? std::format("f{}({})", std::get<c::FunctionId>(op.attribute).value,
                                      args(op.operands))
                        : "[&]() -> " + response(signature) + " {\n" +
                              region(op.regions[0], signature, {}, indent + "    ") + indent +
                              "}()";
                text += std::format("{}[[maybe_unused]] auto call_{} = {};\n", indent, op.id.value,
                                    call);
                if (signature.error)
                    text +=
                        std::format("{}if (!call_{}) return std::unexpected(call_{}.error());\n",
                                    indent, op.id.value, op.id.value);
                for (std::size_t i = 0; i < op.results.size(); ++i)
                    text += declaration(op.results[i],
                                        std::format("std::get<{}>({}call_{})", i,
                                                    signature.error ? "*" : "", op.id.value),
                                        indent);
                continue;
            }
            std::string expression;
            if (op.code == c::OpCode::Constant)
                expression = literal(std::get<c::ConstantId>(op.attribute));
            else if (op.code == c::OpCode::Identity)
                expression = args(op.operands);
            else if (op.code == c::OpCode::Aggregate)
                expression = type(f->values[op.results[0].value]) + "{" + args(op.operands) + "}";
            else if (op.code == c::OpCode::Extract)
                expression = std::format(
                    "v{}.{}", op.operands[0].value,
                    field(f->values[op.operands[0].value], std::get<c::FieldId>(op.attribute)));
            else if (op.code == c::OpCode::Insert) {
                // Physical copies are an emission choice for the functional insert OP.
                auto result = op.results[0];
                text +=
                    std::format("{}[[maybe_unused]] {} v{} = v{};\n{}v{}.{} = v{};\n", indent,
                                type(f->values[result.value]), result.value, op.operands[0].value,
                                indent, result.value,
                                field(f->values[result.value], std::get<c::FieldId>(op.attribute)),
                                op.operands[1].value);
                continue;
            } else if (op.code == c::OpCode::ExternalCall)
                expression =
                    "::" + p.cpp.externalSymbols[std::get<c::ExternalId>(op.attribute).value] +
                    "(" + args(op.operands) + ")";
            else if (auto token = symbol(op.code); !token.empty()) {
                if (op.operands.size() == 1)
                    expression = "(" + token + args(op.operands) + ")";
                else
                    expression = std::format("(v{} {} v{})", op.operands[0].value, token,
                                             op.operands[1].value);
            } else {
                auto target = math(op.code);
                require(!target.empty(), "OP has no C++ lowering");
                expression = target + "(" + args(op.operands) + ")";
            }
            text += declaration(op.results[0], expression, indent);
        }
        if (const auto *term = std::get_if<c::ReturnSuccess>(&*r.terminator))
            text +=
                indent + "return " + tuple(boundary.results) + "{" + args(term->values) + "};\n";
        else if (const auto *term = std::get_if<c::ReturnError>(&*r.terminator))
            text += std::format("{}return std::unexpected(v{});\n", indent, term->error.value);
        else if (const auto *term = std::get_if<c::Yield>(&*r.terminator))
            for (std::size_t i = 0; i < yields.size(); ++i)
                text +=
                    std::format("{}v{} = v{};\n", indent, yields[i].value, term->values[i].value);
        return text;
    }
    std::string signature(std::size_t id) const {
        const auto &fn = m.functions[id];
        std::string parameters;
        for (auto param : fn.parameters) {
            if (!parameters.empty())
                parameters += ", ";
            parameters +=
                std::format("[[maybe_unused]] {} v{}", type(fn.values[param.value]), param.value);
        }
        return std::format("inline {} f{}({})", response(fn.signature), id, parameters);
    }
    std::string exportObject(const Export &e) const {
        const auto &fn = m.functions[e.function.value];
        const auto &name = e.name;
        std::string text = "struct " + name + "_context {\n";
        const auto &bindings = p.bindings.functions[e.function.value];
        for (const auto &b : bindings)
            text += "    " + type(fn.signature.parameters[b.parameter]) + " " + b.field + ";\n";
        text += "};\n";
        if (directEvent(e))
            text += "using " + name +
                    "_event = " + type(fn.signature.parameters[e.eventParameters[0]]) + ";\n";
        else {
            text += "struct " + name + "_event {\n";
            for (std::size_t i = 0; i < e.eventParameters.size(); ++i)
                text += "    " + type(fn.signature.parameters[e.eventParameters[i]]) + " " +
                        e.eventFields[i] + ";\n";
            text += "};\n";
        }
        if (!e.stateParameter)
            text += "struct " + name + "_state {};\n";
        else if (record(fn.signature.parameters[*e.stateParameter]).name != name + "_state")
            text += "using " + name +
                    "_state = " + type(fn.signature.parameters[*e.stateParameter]) + ";\n";
        if (fn.signature.error)
            text += "using " + name + "_error = " + type(*fn.signature.error) + ";\n";
        else
            text += "struct " + name + "_error {};\n";
        text += "using " + name + "_result = " + type(fn.signature.results[0]) + ";\n";
        text += std::format(
            "using {0}_contract = dsl_runtime::contract<{0}_context, {0}_state, {0}_event, "
            "{0}_result, {0}_error>;\nusing {0}_output = {0}_contract::output;\n",
            name);
        text += std::format("inline {0}_context prepare_{0}_context([[maybe_unused]] const "
                            "{0}_event& event) {{\n    return {{",
                            name);
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            const auto &b = bindings[i];
            if (i)
                text += ", ";
            text += "." + b.field + " = ::" + p.cpp.externalSymbols[b.provider.value] + "(";
            for (std::size_t j = 0; j < b.arguments.size(); ++j) {
                if (j)
                    text += ", ";
                text += bindingArgument(e, b.arguments[j]);
            }
            text += ")";
        }
        text += "};\n}\n";
        text += std::format("struct {0} {{\n    using contract_t = {0}_contract;\n    "
                            "[[nodiscard]] contract_t::response operator()([[maybe_unused]] const "
                            "{0}_context& ctx, [[maybe_unused]] const {0}_state& state, "
                            "[[maybe_unused]] const {0}_event& event) const {{\n",
                            name);
        std::vector<std::string> arguments(fn.parameters.size());
        for (auto param : e.eventParameters)
            arguments[param] = eventArgument(e, param);
        if (e.stateParameter)
            arguments[*e.stateParameter] = "state";
        for (const auto &b : bindings)
            arguments[b.parameter] = "ctx." + b.field;
        text += std::format("        auto result = ::{}::f{}(", moduleNamespace, e.function.value);
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i)
                text += ", ";
            text += arguments[i];
        }
        text += ");\n";
        if (fn.signature.error)
            text += "        if (!result) return std::unexpected(result.error());\n";
        auto tupleValue = fn.signature.error ? "*result" : "result";
        text += "        return " + name + "_output{.result = std::get<0>(" + tupleValue +
                "), .new_state = ";
        text += e.stateParameter ? std::string("std::get<1>(") + tupleValue + ")" : "{}";
        text += "};\n    }\n};\n";
        for (const auto *suffix : {"context", "event", "state", "result", "error", "output"})
            text += std::format("static_assert(std::is_standard_layout_v<{0}_{1}> && "
                                "std::is_trivially_copyable_v<{0}_{1}>);\n",
                                name, suffix);
        return text;
    }

    std::string exportGroup(const UnitGroup &g) const {
        const auto &first = entry(g.entries.front());
        std::string text = "struct " + g.name + " : ";
        for (std::size_t i = 0; i < g.entries.size(); ++i) {
            if (i)
                text += ", ";
            text += entry(g.entries[i]).name;
        }
        text += " {\n    using contract_t = dsl_runtime::contract_set<" + first.name + "_state";
        for (auto id : g.entries)
            text += ", " + entry(id).name + "_contract";
        text +=
            ">;\n    template <class Event> using contract_for = contract_t::for_event<Event>;\n";
        for (auto id : g.entries)
            text += "    using " + entry(id).name + "::operator();\n";
        text += "};\n";
        for (auto id : g.entries) {
            const auto &e = entry(id);
            text += "inline " + e.name + "_context prepare_" + g.name + "_context(const " + e.name +
                    "_event& event) { return prepare_" + e.name + "_context(event); }\n";
        }
        return text;
    }

  public:
    explicit Emitter(const Program &program) : p(program), m(program.computation) {}
    std::string run() {
        integration();
        // Coexisting bundles must have disjoint public exports. Their lexicographically
        // smallest export therefore gives each bundle a distinct, deterministic anchor.
        // No hashes, source paths, emission order or process-global counters are needed.
        const auto anchor = std::ranges::min_element(p.units.exports, {}, &Export::name);
        moduleNamespace = "dsl_backend::module_" + anchor->name;
        std::string text =
            "// Generated from verified backend-independent computation IR. C++23.\n";
        if (p.units.style == UnitEnvelope::Style::Objects)
            text += "#pragma once\n";
        text += "#include <dsl_runtime/math.h>\n#include <dsl_runtime/operation.h>\n#include "
                "<tuple>\n#include <bit>\n#include <cstdint>\n";
        for (const auto &h : p.cpp.headers)
            text += "#include \"" + h + "\"\n";
        for (const auto &name : p.cpp.macros)
            text += "#undef " + name + "\n";
        for (const auto &r : p.cpp.records)
            if (!r.external) {
                text += "struct " + r.name + " {\n";
                const auto &fields = std::get<c::RecordType>(m.types[r.type.value]).fields;
                const auto &values =
                    std::get<std::vector<c::ConstantId>>(m.constants[r.initial.value].payload);
                for (std::size_t i = 0; i < r.fields.size(); ++i)
                    text += "    " + type(fields[i].type) + " " + r.fields[i] + " = " +
                            literal(values[i]) + ";\n";
                text += "};\n";
            }
        text += "namespace " + moduleNamespace + " {\n";
        for (std::size_t i = 0; i < m.functions.size(); ++i)
            text += signature(i) + ";\n";
        for (std::size_t i = 0; i < m.functions.size(); ++i) {
            f = &m.functions[i];
            text += signature(i) + " {\n" + region(f->body, f->signature, {}, "    ") + "}\n";
        }
        text += "}\n";
        for (const auto &e : p.units.exports) {
            if (p.units.style == UnitEnvelope::Style::Objects)
                text += exportObject(e);
            else {
                const auto &fn = m.functions[e.function.value];
                text += type(fn.signature.results[0]) + " " + e.name + "(";
                for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
                    if (i)
                        text += ", ";
                    text += type(fn.signature.parameters[i]) + " arg" + std::to_string(i);
                }
                text += ") { return std::get<0>(::" + moduleNamespace + "::f" +
                        std::to_string(e.function.value) + "(";
                for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
                    if (i)
                        text += ", ";
                    text += "arg" + std::to_string(i);
                }
                text += ")); }\n";
            }
        }
        for (const auto &g : p.units.groups)
            text += exportGroup(g);
        return text;
    }
};
} // namespace
std::expected<std::string, std::string> generate(const Program &p) {
    if (auto valid = c::verify(p.computation); !valid)
        return std::unexpected(valid.error());
    try {
        return Emitter(p).run();
    } catch (const std::exception &e) {
        return std::unexpected(e.what());
    }
}
} // namespace dsl::cpp
