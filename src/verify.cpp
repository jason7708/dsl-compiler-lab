#include "dsl/verify.h"
#include "scoped_context.h"
#include <algorithm>
#include <format>
#include <set>
#include <stdexcept>
namespace dsl::ir {
namespace {
struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};
class Verifier {
    const Module &m;
    std::span<const OpDefinition> definitions;
    const Function *f = nullptr;
    std::vector<bool> defined;
    std::set<std::uint32_t> operations;
    std::string where = "module";
    void require(bool condition, std::string_view reason) const {
        if (!condition)
            throw Invalid(where + ": IR verification: " + std::string(reason));
    }
    bool valid(TypeId t) const { return t.value < m.types.size(); }
    void type(TypeId t) const { require(valid(t), "unknown TypeId"); }
    TypeId value(ValueId v) const {
        require(v.value < f->values.size(), "ValueId out of range");
        return f->values[v.value];
    }
    void signature(const Signature &s) {
        for (auto t : s.parameters)
            type(t);
        for (auto t : s.results)
            type(t);
        if (s.error)
            type(*s.error);
    }
    void same(const std::vector<ValueId> &ids, const std::vector<TypeId> &types) {
        require(ids.size() == types.size(), "arity mismatch");
        for (std::size_t i = 0; i < ids.size(); ++i)
            require(value(ids[i]) == types[i], "type mismatch");
    }
    const RecordType &record(TypeId t) {
        type(t);
        const auto *r = std::get_if<RecordType>(&m.types[t.value]);
        require(r != nullptr, "record type required");
        return *r;
    }
    const Field &field(TypeId t, FieldId id) {
        const auto &r = record(t);
        const auto it = std::ranges::find(r.fields, id, &Field::id);
        require(it != r.fields.end(), "unknown FieldId");
        return *it;
    }
    template <class T> const T &attribute(const Operation &op) {
        const auto *a = std::get_if<T>(&op.attribute);
        require(a != nullptr, "invalid operation attribute");
        return *a;
    }
    void typesAndConstants() {
        std::vector<int> visiting(m.types.size());
        auto visitType = [&](auto &&self, TypeId id, unsigned depth) -> void {
            type(id);
            require(depth < 256, "type nesting limit exceeded");
            require(visiting[id.value] != 1, "recursive by-value type");
            if (visiting[id.value] == 2)
                return;
            visiting[id.value] = 1;
            const auto &t = m.types[id.value];
            if (const auto *integer = std::get_if<IntegerType>(&t)) {
                require(integer->width > 0 && integer->width <= 64, "unsupported integer width");
                require(integer->signedness == Signedness::Signed ||
                            integer->signedness == Signedness::Unsigned,
                        "invalid signedness");
            } else if (const auto *fp = std::get_if<FloatType>(&t)) {
                require(fp->format == FloatFormat::IEEE754Binary32 ||
                            fp->format == FloatFormat::IEEE754Binary64,
                        "invalid numeric format");
            } else if (const auto *r = std::get_if<RecordType>(&t)) {
                std::set<std::uint32_t> ids;
                for (const auto &leaf : r->fields) {
                    require(ids.insert(leaf.id.value).second, "duplicate FieldId");
                    self(self, leaf.type, depth + 1);
                }
            }
            visiting[id.value] = 2;
        };
        for (std::uint32_t i = 0; i < m.types.size(); ++i)
            visitType(visitType, TypeId{i}, 0);
        std::vector<int> constants(m.constants.size());
        auto visitConstant = [&](auto &&self, ConstantId id, unsigned depth) -> void {
            require(id.value < m.constants.size(), "unknown ConstantId");
            require(depth < 256, "constant nesting limit exceeded");
            require(constants[id.value] != 1, "cyclic constant");
            if (constants[id.value] == 2)
                return;
            constants[id.value] = 1;
            const auto &c = m.constants[id.value];
            type(c.type);
            if (const auto *r = std::get_if<RecordType>(&m.types[c.type.value])) {
                const auto *fields = std::get_if<std::vector<ConstantId>>(&c.payload);
                require(fields && fields->size() == r->fields.size(),
                        "aggregate constant arity mismatch");
                for (std::size_t i = 0; i < fields->size(); ++i) {
                    self(self, (*fields)[i], depth + 1);
                    require(m.constants[(*fields)[i].value].type == r->fields[i].type,
                            "aggregate constant field type mismatch");
                }
            } else {
                const auto *bits = std::get_if<std::uint64_t>(&c.payload);
                require(bits != nullptr, "scalar constant needs numeric bits");
                unsigned width = 64;
                if (std::holds_alternative<BoolType>(m.types[c.type.value]))
                    width = 1;
                if (const auto *i = std::get_if<IntegerType>(&m.types[c.type.value]))
                    width = i->width;
                if (const auto *fp = std::get_if<FloatType>(&m.types[c.type.value]);
                    fp && fp->format == FloatFormat::IEEE754Binary32)
                    width = 32;
                require(width == 64 || (*bits >> width) == 0, "constant bits exceed numeric width");
            }
            constants[id.value] = 2;
        };
        for (std::uint32_t i = 0; i < m.constants.size(); ++i)
            visitConstant(visitConstant, ConstantId{i}, 0);
    }
    // Return true when this region can reach its enclosing continuation (Yield).
    bool region(const Region &r, std::vector<bool> available, const Signature &boundary,
                const std::optional<std::vector<TypeId>> &yields, unsigned depth) {
        detail::ScopedContext regionContext(
            where, r.location.empty() ? where : f->debugName + " at " + r.location);
        require(depth < 256, "region nesting limit exceeded");
        bool reachable = true;
        auto use = [&](ValueId id) {
            (void)value(id);
            require(available[id.value], "value does not dominate use or escapes its region");
        };
        for (const auto &op : r.operations) {
            detail::ScopedContext operationContext(
                where, std::format("{} op #{}{}", f->debugName, op.id.value,
                                   op.location.empty() ? "" : " at " + op.location));
            require(reachable, "operation after unconditional termination");
            require(operations.insert(op.id.value).second, "duplicate OperationId");
            const auto *definition = lookup(op.code, definitions);
            require(definition != nullptr, "unregistered operation");
            for (auto id : op.operands)
                use(id);
            for (auto id : op.results) {
                (void)value(id);
                require(!defined[id.value], "value defined more than once");
            }
            auto arity = [&](std::size_t inputs, std::size_t outputs) {
                require(op.operands.size() == inputs && op.results.size() == outputs,
                        "operation arity mismatch");
            };
            auto resultTypes = [&] {
                std::vector<TypeId> types;
                for (auto id : op.results)
                    types.push_back(value(id));
                return types;
            };
            using enum Rule;
            const auto rule = definition->rule;
            if (rule != If && rule != Scope && rule != Evaluate)
                require(op.regions.empty(), "unexpected nested regions");
            if (rule != Constant && rule != Extract && rule != Insert && rule != Call &&
                rule != ExternalCall && rule != Evaluate)
                (void)attribute<std::monostate>(op);
            switch (rule) {
            case Constant: {
                arity(0, 1);
                auto id = attribute<ConstantId>(op);
                require(id.value < m.constants.size(), "unknown ConstantId");
                require(value(op.results[0]) == m.constants[id.value].type,
                        "constant result type mismatch");
                break;
            }
            case Identity:
                arity(1, 1);
                require(value(op.results[0]) == value(op.operands[0]), "identity type mismatch");
                break;
            case FloatBinary:
            case FloatUnary:
            case BoolUnary: {
                arity(rule == FloatBinary ? 2 : 1, 1);
                auto t = value(op.operands[0]);
                require(rule == BoolUnary ? std::holds_alternative<BoolType>(m.types[t.value])
                                          : std::holds_alternative<FloatType>(m.types[t.value]),
                        "numeric operand type mismatch");
                for (auto id : op.operands)
                    require(value(id) == t, "operand types must match");
                require(value(op.results[0]) == t, "numeric result type mismatch");
                break;
            }
            case Compare: {
                arity(2, 1);
                auto t = value(op.operands[0]);
                require(value(op.operands[1]) == t, "comparison operands must match");
                require(std::holds_alternative<FloatType>(m.types[t.value]) ||
                            std::holds_alternative<IntegerType>(m.types[t.value]),
                        "comparison requires numeric operands");
                require(std::holds_alternative<BoolType>(m.types[value(op.results[0]).value]),
                        "comparison requires bool result");
                break;
            }
            case Aggregate: {
                require(op.results.size() == 1, "aggregate needs one result");
                const auto &fields = record(value(op.results[0])).fields;
                require(op.operands.size() == fields.size(), "aggregate arity mismatch");
                for (std::size_t i = 0; i < fields.size(); ++i)
                    require(value(op.operands[i]) == fields[i].type,
                            "aggregate field type mismatch");
                break;
            }
            case Extract:
            case Insert: {
                arity(rule == Extract ? 1 : 2, 1);
                auto fieldId = attribute<FieldId>(op);
                auto base = value(op.operands[0]);
                auto leaf = field(base, fieldId).type;
                require(value(op.results[0]) == (rule == Extract ? leaf : base),
                        "record operation result type mismatch");
                if (rule == Insert)
                    require(value(op.operands[1]) == leaf, "insert field type mismatch");
                break;
            }
            case Call:
            case ExternalCall: {
                const Signature *s;
                if (rule == Call) {
                    auto id = attribute<FunctionId>(op);
                    require(id.value < m.functions.size(), "unknown FunctionId");
                    s = &m.functions[id.value].signature;
                } else {
                    auto id = attribute<ExternalId>(op);
                    require(id.value < m.externals.size(), "unknown ExternalId");
                    s = &m.externals[id.value].signature;
                }
                same(op.operands, s->parameters);
                same(op.results, s->results);
                require(!s->error || s->error == boundary.error, "unhandled call error type");
                break;
            }
            case If:
            case Scope: {
                require(op.operands.size() == (rule == If ? 1 : 0),
                        "control operand arity mismatch");
                if (rule == If)
                    require(std::holds_alternative<BoolType>(m.types[value(op.operands[0]).value]),
                            "if condition must be bool");
                require(op.regions.size() == (rule == If ? 2 : 1), "control region arity mismatch");
                bool continues = false;
                for (const auto &child : op.regions)
                    continues =
                        region(child, available, boundary, resultTypes(), depth + 1) || continues;
                reachable = continues;
                break;
            }
            case Evaluate: {
                arity(0, op.results.size());
                require(op.regions.size() == 1, "evaluate needs one region");
                auto error = attribute<Evaluation>(op).error;
                if (error)
                    type(*error);
                require(!error || error == boundary.error, "unhandled evaluation error type");
                region(op.regions[0], available, Signature{{}, resultTypes(), error}, std::nullopt,
                       depth + 1);
                break;
            }
            default:
                require(false, "unknown OP verification rule");
            }
            for (auto id : op.results) {
                require(!defined[id.value], "value defined more than once");
                available[id.value] = defined[id.value] = true;
            }
        }
        detail::ScopedContext terminatorContext(
            where,
            r.terminatorLocation.empty() ? where : f->debugName + " at " + r.terminatorLocation);
        require(r.terminator.has_value(), "missing terminator");
        if (const auto *term = std::get_if<Yield>(&*r.terminator)) {
            require(reachable && yields.has_value(), "yield outside a yielding region");
            for (auto id : term->values)
                use(id);
            same(term->values, *yields);
            return true;
        }
        if (const auto *term = std::get_if<ReturnSuccess>(&*r.terminator)) {
            require(reachable, "return after unconditional termination");
            for (auto id : term->values)
                use(id);
            same(term->values, boundary.results);
        } else if (const auto *term = std::get_if<ReturnError>(&*r.terminator)) {
            require(reachable, "error after unconditional termination");
            use(term->error);
            require(boundary.error && value(term->error) == *boundary.error, "error type mismatch");
        } else
            require(!reachable, "unreachable terminator needs a terminating predecessor");
        return false;
    }

  public:
    Verifier(const Module &module, std::span<const OpDefinition> registry)
        : m(module), definitions(registry) {}
    void run() {
        std::set<OpCode> registered;
        for (const auto &d : definitions)
            require(registered.insert(d.code).second, "duplicate OP registration");
        typesAndConstants();
        for (const auto &e : m.externals) {
            detail::ScopedContext context(where, e.debugName);
            signature(e.signature);
        }
        for (const auto &fn : m.functions) {
            detail::ScopedContext context(where, fn.debugName);
            signature(fn.signature);
        }
        for (const auto &fn : m.functions) {
            f = &fn;
            detail::ScopedContext context(where, fn.debugName);
            for (auto t : fn.values)
                type(t);
            defined.assign(fn.values.size(), false);
            operations.clear();
            same(fn.parameters, fn.signature.parameters);
            for (auto id : fn.parameters) {
                require(!defined[id.value], "duplicate parameter");
                defined[id.value] = true;
            }
            region(fn.body, defined, fn.signature, std::nullopt, 0);
            require(std::ranges::all_of(defined, [](bool x) { return x; }),
                    "value has no definition");
        }
    }
};
} // namespace
std::expected<void, std::string> verify(const Module &m,
                                        std::span<const OpDefinition> definitions) {
    try {
        Verifier(m, definitions).run();
        return {};
    } catch (const Invalid &error) {
        return std::unexpected(error.what());
    }
}
} // namespace dsl::ir
