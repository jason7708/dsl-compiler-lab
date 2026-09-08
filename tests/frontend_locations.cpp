#include "dsl/frontend.h"
#include "dsl/verify.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace dsl;
namespace {
ir::Operation *find(ir::Region &region, ir::OpCode code) {
    for (auto &op : region.operations) {
        if (op.code == code)
            return &op;
        for (auto &child : op.regions)
            if (auto *result = find(child, code))
                return result;
    }
    return nullptr;
}
ir::Function &function(Program &p, std::string_view name) {
    auto it = std::ranges::find(p.computation.functions, name, &ir::Function::debugName);
    if (it == p.computation.functions.end())
        throw std::runtime_error("missing function");
    return *it;
}
Program lower(std::string_view source, bool objects = false) {
    auto p = compile(source, "locations.dsl.cpp", {.objects = objects});
    if (!p)
        throw std::runtime_error(p.error());
    return std::move(*p);
}
} // namespace
int main() {
    int cases = 0;
    auto check = [&](bool ok, std::string_view message) {
        ++cases;
        if (!ok)
            throw std::runtime_error(std::string(message));
    };
    try {
        auto p = lower("double compute(double a, double b) {\n"
                       "    const double sum = a + b;\n"
                       "    return sum * 0.5;\n"
                       "}\n");
        auto &f = function(p, "compute");
        auto *add = find(f.body, ir::OpCode::Add);
        auto *mul = find(f.body, ir::OpCode::Multiply);
        check(add && add->location.ends_with("locations.dsl.cpp:2:26"),
              "add lost its operator location");
        check(mul && mul->location.ends_with("locations.dsl.cpp:3:16"),
              "multiply inherited an operand/function location");
        check(f.body.terminatorLocation.ends_with("locations.dsl.cpp:3:5"),
              "scalar return lost its location");
        add->operands[1] = {999};
        auto invalid = ir::verify(p.computation);
        check(!invalid && invalid.error().find("locations.dsl.cpp:2:26") != std::string::npos,
              "verifier did not report the actual bad source operator");

        p = lower("double child(double a, double b) {\n"
                  "    return a + b;\n"
                  "}\n"
                  "double parent(double x) {\n"
                  "    double y = child(x, 2.0);\n"
                  "    return y * 3.0;\n"
                  "}\n",
                  true);
        auto &parent = function(p, "parent");
        add = find(parent.body, ir::OpCode::Add);
        mul = find(parent.body, ir::OpCode::Multiply);
        check(add && add->location.ends_with("locations.dsl.cpp:2:14"),
              "inlined child lost original operator location");
        check(mul && mul->location.ends_with("locations.dsl.cpp:6:14"),
              "parent location was not restored after child lowering");
        check(parent.body.terminatorLocation.ends_with("locations.dsl.cpp:6:5"),
              "object return lost its location");
        add->operands[1] = {999};
        invalid = ir::verify(p.computation);
        check(!invalid && invalid.error().find("locations.dsl.cpp:2:14") != std::string::npos,
              "inlined verifier error points to parent instead of child source");

        p = lower("double f(bool c, double v) {\n"
                  "    return c ? v : 0.0;\n"
                  "}\n",
                  true);
        auto *choice = find(function(p, "f").body, ir::OpCode::If);
        check(choice && choice->regions[0].operations.empty(),
              "expected an instruction-free reference branch");
        check(choice->regions[0].terminatorLocation.ends_with("locations.dsl.cpp:2:16"),
              "empty branch lost the source expression location");
        choice->regions[0].terminator = ir::Yield{{ir::ValueId{999}}};
        invalid = ir::verify(p.computation);
        check(!invalid && invalid.error().find("locations.dsl.cpp:2:16") != std::string::npos,
              "empty branch diagnostic points to a previous operator");
        std::cout << cases << " frontend source-location cases passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
