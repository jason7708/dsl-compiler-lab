#include "dsl/verify.h"
#include <functional>
#include <iostream>
#include <type_traits>
using namespace dsl::ir;
static_assert(!std::is_convertible_v<TypeId, ValueId>);
static_assert(!std::is_convertible_v<OperationId, ValueId>);
Module arithmetic() {
    Module m;
    m.types = {FloatType{FloatFormat::IEEE754Binary64}, BoolType{},
               IntegerType{32, Signedness::Signed}, RecordType{{{{0}, {0}}, {{1}, {1}}}}};
    m.constants = {{{0}, std::uint64_t{0x3ff0000000000000}},
                   {{1}, std::uint64_t{1}},
                   {{3}, std::vector<ConstantId>{{0}, {1}}}};
    Function f{"add", {{{0}, {0}}, {{0}}, std::nullopt}, {{0}, {0}, {0}}, {{0}, {1}}, {}};
    f.body.operations.push_back({{0}, OpCode::Add, {{0}, {1}}, {{2}}, {}, {}, "fixture"});
    f.body.terminator = ReturnSuccess{{{2}}};
    m.functions.push_back(std::move(f));
    return m;
}
Module branches() {
    auto m = arithmetic();
    auto &f = m.functions[0];
    f.values = {{0}, {0}, {1}, {0}, {0}, {0}};
    f.body.operations = {{{0}, OpCode::Constant, {}, {{2}}, ConstantId{1}, {}, "condition"}};
    Region yes{{{{1}, OpCode::Add, {{0}, {1}}, {{3}}, {}, {}, "yes"}}, Yield{{{3}}}};
    Region no{{{{2}, OpCode::Identity, {{1}}, {{4}}, {}, {}, "no"}}, Yield{{{4}}}};
    f.body.operations.push_back({{3}, OpCode::If, {{2}}, {{5}}, {}, {yes, no}, "branch"});
    f.body.terminator = ReturnSuccess{{{5}}};
    return m;
}
int main() {
    int cases = 0;
    auto good = [&](std::string name, const Module &m) {
        ++cases;
        auto result = verify(m);
        if (!result)
            throw std::runtime_error(name + ": " + result.error());
    };
    auto bad = [&](std::string name, Module m, const std::function<void(Module &)> &mutate,
                   std::string_view reason) {
        ++cases;
        mutate(m);
        auto result = verify(m);
        if (result || result.error().find(reason) == std::string::npos)
            throw std::runtime_error(name + ": " +
                                     (result ? "accepted invalid IR" : result.error()));
    };
    try {
        good("arithmetic without any backend metadata", arithmetic());
        good("legal outer-region captures", branches());
        auto fp32 = arithmetic();
        fp32.types[0] = FloatType{FloatFormat::IEEE754Binary32};
        fp32.constants[0].payload = std::uint64_t{0x3f800000};
        good("backend independent binary32 type", fp32);
        auto unsigned32 = arithmetic();
        unsigned32.types[2] = IntegerType{32, Signedness::Unsigned};
        good("unsigned type", unsigned32);
        bad(
            "invalid value", arithmetic(),
            [](Module &m) { m.functions[0].body.operations[0].operands[0] = {99}; },
            "ValueId out of range");
        bad(
            "use before definition", arithmetic(),
            [](Module &m) { m.functions[0].body.operations[0].operands[0] = {2}; }, "dominate");
        bad(
            "bad result id", arithmetic(),
            [](Module &m) { m.functions[0].body.operations[0].results[0] = {99}; },
            "ValueId out of range");
        bad(
            "duplicate parameter", arithmetic(),
            [](Module &m) { m.functions[0].parameters[1] = {0}; }, "duplicate parameter");
        bad(
            "redefine parameter", arithmetic(),
            [](Module &m) { m.functions[0].body.operations[0].results[0] = {0}; },
            "defined more than once");
        bad(
            "unowned value", arithmetic(), [](Module &m) { m.functions[0].values.push_back({0}); },
            "no definition");
        bad(
            "unknown value type", arithmetic(), [](Module &m) { m.functions[0].values[0] = {99}; },
            "TypeId");
        bad(
            "operand types", arithmetic(),
            [](Module &m) {
                m.functions[0].values[1] = {1};
                m.functions[0].signature.parameters[1] = {1};
            },
            "operand types");
        bad(
            "wrong result type", arithmetic(), [](Module &m) { m.functions[0].values[2] = {1}; },
            "result type");
        bad(
            "wrong arity", arithmetic(),
            [](Module &m) { m.functions[0].body.operations[0].operands.pop_back(); }, "arity");
        bad(
            "unregistered opcode", arithmetic(),
            [](Module &m) { m.functions[0].body.operations[0].code = static_cast<OpCode>(999); },
            "unregistered");
        bad(
            "unexpected attribute", arithmetic(),
            [](Module &m) { m.functions[0].body.operations[0].attribute = FieldId{0}; },
            "attribute");
        bad(
            "missing terminator", arithmetic(),
            [](Module &m) { m.functions[0].body.terminator.reset(); }, "missing terminator");
        bad(
            "bad return", arithmetic(),
            [](Module &m) { m.functions[0].body.terminator = ReturnSuccess{{{0}, {1}}}; }, "arity");
        bad(
            "root yield", arithmetic(),
            [](Module &m) { m.functions[0].body.terminator = Yield{{{2}}}; }, "yield outside");
        bad(
            "unhandled error", arithmetic(),
            [](Module &m) { m.functions[0].body.terminator = ReturnError{{2}}; }, "error type");
        bad(
            "invented unreachable", arithmetic(),
            [](Module &m) { m.functions[0].body.terminator = Unreachable{}; },
            "terminating predecessor");
        bad(
            "integer width", arithmetic(),
            [](Module &m) { m.types[2] = IntegerType{0, Signedness::Signed}; }, "width");
        bad(
            "signedness", arithmetic(),
            [](Module &m) { m.types[2] = IntegerType{32, static_cast<Signedness>(99)}; },
            "signedness");
        bad(
            "numeric format", arithmetic(),
            [](Module &m) { m.types[0] = FloatType{static_cast<FloatFormat>(99)}; },
            "numeric format");
        bad(
            "recursive record", arithmetic(),
            [](Module &m) { m.types[3] = RecordType{{{{0}, {3}}}}; }, "recursive");
        bad(
            "duplicate fields", arithmetic(),
            [](Module &m) { std::get<RecordType>(m.types[3]).fields[1].id = {0}; },
            "duplicate FieldId");
        bad(
            "unknown field type", arithmetic(),
            [](Module &m) { std::get<RecordType>(m.types[3]).fields[1].type = {99}; }, "TypeId");
        bad(
            "constant bits", arithmetic(),
            [](Module &m) { m.constants[1].payload = std::uint64_t{2}; }, "numeric width");
        bad(
            "constant arity", arithmetic(),
            [](Module &m) { m.constants[2].payload = std::vector<ConstantId>{}; },
            "constant arity");
        bad(
            "constant field type", arithmetic(),
            [](Module &m) { m.constants[2].payload = std::vector<ConstantId>{{1}, {0}}; },
            "constant field type");
        bad(
            "constant cycle", arithmetic(),
            [](Module &m) { m.constants[2].payload = std::vector<ConstantId>{{2}, {1}}; },
            "cyclic constant");
        bad(
            "unknown constant", arithmetic(),
            [](Module &m) { m.constants[2].payload = std::vector<ConstantId>{{99}, {1}}; },
            "ConstantId");
        bad(
            "sibling capture", branches(),
            [](Module &m) {
                m.functions[0].body.operations[1].regions[1].operations[0].operands[0] = {3};
            },
            "dominate");
        bad(
            "nested result escapes", branches(),
            [](Module &m) { m.functions[0].body.terminator = ReturnSuccess{{{3}}}; }, "dominate");
        bad(
            "bad yield", branches(),
            [](Module &m) {
                m.functions[0].body.operations[1].regions[0].terminator = Yield{{{2}}};
            },
            "type mismatch");
        bad(
            "missing branch terminator", branches(),
            [](Module &m) { m.functions[0].body.operations[1].regions[0].terminator.reset(); },
            "missing terminator");
        bad(
            "missing else", branches(),
            [](Module &m) { m.functions[0].body.operations[1].regions.pop_back(); },
            "region arity");
        bad(
            "duplicate operation id", branches(),
            [](Module &m) { m.functions[0].body.operations[1].id = {0}; }, "OperationId");
        bad(
            "nonbool condition", branches(),
            [](Module &m) { m.functions[0].body.operations[1].operands[0] = {0}; }, "bool");
        auto returns = branches();
        auto &op = returns.functions[0].body.operations[1];
        op.regions[0].terminator = ReturnSuccess{{{3}}};
        op.regions[1].terminator = ReturnSuccess{{{4}}};
        returns.functions[0].body.terminator = Unreachable{};
        good("all branches terminate", returns);
        bad(
            "operation after termination", returns,
            [](Module &m) {
                auto &f = m.functions[0];
                f.values.push_back({0});
                f.body.operations.push_back({{4}, OpCode::Identity, {{0}}, {{6}}, {}, {}, "after"});
            },
            "unconditional termination");
        bad(
            "unknown callee", arithmetic(),
            [](Module &m) {
                auto &o = m.functions[0].body.operations[0];
                o.code = OpCode::Call;
                o.attribute = FunctionId{99};
            },
            "FunctionId");
        auto call = arithmetic();
        call.functions[0].body.operations[0].code = OpCode::Call;
        call.functions[0].body.operations[0].attribute = FunctionId{0};
        good("recursive call signature", call);
        bad(
            "call signature", call,
            [](Module &m) { m.functions[0].body.operations[0].operands.pop_back(); }, "arity");
        bad(
            "unknown external", arithmetic(),
            [](Module &m) {
                auto &o = m.functions[0].body.operations[0];
                o.code = OpCode::ExternalCall;
                o.attribute = ExternalId{0};
            },
            "ExternalId");
        auto field = arithmetic();
        field.functions[0].signature.parameters = {TypeId{3}, TypeId{0}};
        field.functions[0].values[0] = {3};
        field.functions[0].body.operations[0] = {
            OperationId{0}, OpCode::Extract, {{0}}, {{2}}, FieldId{0}, {}, "extract"};
        good("record extraction", field);
        bad(
            "missing field", field,
            [](Module &m) { m.functions[0].body.operations[0].attribute = FieldId{2}; }, "FieldId");
        bad(
            "insert field type", field,
            [](Module &m) {
                auto &f = m.functions[0];
                f.values[2] = {3};
                f.signature.results[0] = {3};
                f.body.operations[0] = {
                    OperationId{0}, OpCode::Insert, {{0}, {1}}, {{2}}, FieldId{1}, {}, "insert"};
            },
            "insert field type");
        bad(
            "operator source diagnostic", arithmetic(),
            [](Module &m) {
                auto &op = m.functions[0].body.operations[0];
                op.location = "user.dsl.cpp:8:19";
                op.operands[0] = {99};
            },
            "add op #0 at user.dsl.cpp:8:19");
        bad(
            "nested operator source diagnostic", branches(),
            [](Module &m) {
                auto &op = m.functions[0].body.operations[1].regions[0].operations[0];
                op.location = "helper.dsl.h:12:9";
                op.operands[0] = {99};
            },
            "add op #1 at helper.dsl.h:12:9");
        bad(
            "parent diagnostic restored after region", branches(),
            [](Module &m) {
                auto &op = m.functions[0].body.operations[1];
                op.location = "parent.dsl.cpp:9:5";
                // The nested definition is valid inside its branch, but cannot also
                // define the enclosing operation result. Checked after the recursion.
                op.results[0] = {3};
            },
            "add op #3 at parent.dsl.cpp:9:5");
        bad(
            "empty region retains its own diagnostic", branches(),
            [](Module &m) {
                auto &r = m.functions[0].body.operations[1].regions[1];
                r.operations.clear();
                r.terminator.reset();
                r.location = "branch.dsl.cpp:11:7";
            },
            "add at branch.dsl.cpp:11:7");
        bad(
            "terminator has independent location", arithmetic(),
            [](Module &m) {
                auto &r = m.functions[0].body;
                r.terminatorLocation = "return.dsl.cpp:14:5";
                r.terminator = ReturnSuccess{{{99}}};
            },
            "add at return.dsl.cpp:14:5");
        ++cases;
        if (verify(arithmetic(), {}))
            throw std::runtime_error("missing registry accepted");
        std::cout << cases << " independent IR verifier cases passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
