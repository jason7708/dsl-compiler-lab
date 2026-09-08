#include "dsl/codegen.h"
#include "dsl/verify.h"
#include <iostream>
using namespace dsl;
Program program() {
    Program p;
    p.computation.types = {ir::FloatType{ir::FloatFormat::IEEE754Binary64}};
    ir::Function f{"source-independent", {{{0}}, {{0}}, std::nullopt}, {{0}}, {{0}}, {}};
    f.body.terminator = ir::ReturnSuccess{{{0}}};
    p.computation.functions.push_back(f);
    p.bindings.functions.resize(1);
    p.units = {UnitEnvelope::Style::Scalar,
               {{ir::FunctionId{0}, "compute", {0}, {"x"}, std::nullopt, std::nullopt}}};
    return p;
}
Program multiEvent() {
    Program p;
    p.computation.types = {ir::FloatType{ir::FloatFormat::IEEE754Binary64}, ir::BoolType{},
                           ir::RecordType{}};
    p.computation.constants = {{ir::TypeId{2}, std::vector<ir::ConstantId>{}}};
    p.cpp.records = {{ir::TypeId{2}, "Counter_state", {}, false, ir::ConstantId{0}}};
    for (std::uint32_t i = 0; i < 2; ++i) {
        ir::Function f{"entry", {{{i}, {2}}, {{i}, {2}}, std::nullopt}, {{i}, {2}}, {{0}, {1}}, {}};
        f.body.terminator = ir::ReturnSuccess{{{0}, {1}}};
        p.computation.functions.push_back(std::move(f));
        p.units.exports.push_back({ir::FunctionId{i},
                                   "entry_" + std::to_string(i),
                                   {0},
                                   {"event"},
                                   1,
                                   ir::ConstantId{0}});
    }
    p.bindings.functions.resize(2);
    p.units.style = UnitEnvelope::Style::Objects;
    p.units.groups = {{"Counter", {{0}, {1}}}};
    return p;
}
int main() {
    int tests = 0;
    auto require = [&](bool ok, const char *message) {
        ++tests;
        if (!ok)
            throw std::runtime_error(message);
    };
    try {
        auto p = program();
        auto generated = cpp::generate(p);
        require(generated.has_value(), "core-only construction cannot reach C++ backend");
        require(generated->find("double compute(") != std::string::npos, "missing C++ export");
        require(generated->find("namespace dsl_backend::module_compute {") != std::string::npos,
                "missing export-anchored namespace");
        auto noExports = p;
        noExports.units.exports.clear();
        require(!cpp::generate(noExports), "C++ bundle without an export has no module anchor");
        auto renamed = p;
        renamed.units.exports[0].name = "other";
        auto other = cpp::generate(renamed);
        require(other && other->find("namespace dsl_backend::module_other {") != std::string::npos,
                "distinct exports share an implementation namespace");
        auto reordered = p;
        reordered.computation.functions.push_back(reordered.computation.functions[0]);
        reordered.bindings.functions.resize(2);
        reordered.units.exports.push_back(
            {ir::FunctionId{1}, "another", {0}, {"x"}, std::nullopt, std::nullopt});
        auto forward = cpp::generate(reordered);
        std::swap(reordered.units.exports[0], reordered.units.exports[1]);
        auto reverse = cpp::generate(reordered);
        require(forward && reverse &&
                    forward->find("namespace dsl_backend::module_another {") != std::string::npos &&
                    reverse->find("namespace dsl_backend::module_another {") != std::string::npos,
                "module namespace depends on export order");
        p.computation.types[0] = ir::FloatType{ir::FloatFormat::IEEE754Binary32};
        require(ir::verify(p.computation).has_value(),
                "core rejects a format solely because C++ backend lacks support");
        auto unsupported = cpp::generate(p);
        require(!unsupported && unsupported.error().find("binary64") != std::string::npos,
                "backend fails to reject unsupported format");
        p = program();
        p.units.exports[0].eventParameters[0] = 1;
        require(!cpp::generate(p), "invalid event mapping accepted");
        p = program();
        p.bindings.functions.clear();
        require(!cpp::generate(p), "missing bindings table accepted");
        p = program();
        p.units.exports[0].name = "bad-name";
        require(!cpp::generate(p), "invalid C++ name accepted");
        p = program();
        p.units.exports[0].name = "co_return";
        require(!cpp::generate(p), "C++ keyword accepted as export");
        p = program();
        p.units.style = UnitEnvelope::Style::Objects;
        p.computation.functions.push_back(p.computation.functions[0]);
        p.bindings.functions.resize(2);
        p.units.exports.push_back(
            {ir::FunctionId{1}, "compute_context", {0}, {"x"}, std::nullopt, std::nullopt});
        require(!cpp::generate(p), "generated envelope collision accepted");
        p = program();
        p.computation.types.push_back(ir::RecordType{});
        p.computation.types.push_back(ir::RecordType{{{ir::FieldId{0}, ir::TypeId{1}}}});
        p.computation.constants = {{ir::TypeId{1}, std::vector<ir::ConstantId>{}},
                                   {ir::TypeId{2}, std::vector<ir::ConstantId>{{0}}}};
        p.cpp.records = {{ir::TypeId{2}, "Outer", {"inner"}, false, ir::ConstantId{1}},
                         {ir::TypeId{1}, "Inner", {}, false, ir::ConstantId{0}}};
        require(ir::verify(p.computation).has_value(), "core rejects acyclic nested records");
        auto nested = cpp::generate(p);
        require(!nested && nested.error().find("flat records") != std::string::npos,
                "backend emits unsupported nested record layout");
        p = program();
        p.computation.functions[0].body.terminator.reset();
        require(!cpp::generate(p), "backend bypasses verifier");
        p = multiEvent();
        auto multi = cpp::generate(p);
        require(multi && multi->find("struct Counter : entry_0, entry_1") != std::string::npos,
                "source-independent multi-entry envelope failed");
        p.units.groups[0].entries[1] = {99};
        require(!cpp::generate(p), "missing group entry accepted");
        p = multiEvent();
        p.units.groups[0].entries[1] = {0};
        require(!cpp::generate(p), "duplicate group entry accepted");
        p = multiEvent();
        p.computation.functions[1] = p.computation.functions[0];
        require(!cpp::generate(p), "duplicate group event type accepted");
        p = multiEvent();
        p.computation.types.push_back(ir::FloatType{ir::FloatFormat::IEEE754Binary64});
        p.computation.functions[1].signature.parameters[0] = {3};
        p.computation.functions[1].signature.results[0] = {3};
        p.computation.functions[1].values[0] = {3};
        require(!cpp::generate(p), "different TypeIds with the same C++ event type accepted");
        p = multiEvent();
        p.units.groups[0].name = "entry_0";
        require(!cpp::generate(p), "group name collision accepted");
        p = multiEvent();
        p.computation.types.push_back(ir::RecordType{});
        p.computation.constants.push_back({ir::TypeId{3}, std::vector<ir::ConstantId>{}});
        p.cpp.records.push_back({ir::TypeId{3}, "Other_state", {}, false, ir::ConstantId{1}});
        p.computation.functions[1].signature.parameters[1] = {3};
        p.computation.functions[1].signature.results[1] = {3};
        p.computation.functions[1].values[1] = {3};
        p.units.exports[1].initialState = ir::ConstantId{1};
        require(!cpp::generate(p), "group with different state types accepted");
        std::cout << tests << " backend boundary cases passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
