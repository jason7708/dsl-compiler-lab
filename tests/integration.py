#!/usr/bin/env python3
"""Exercise the real CLI, then compile and run its generated C++ with a host compiler."""
import argparse
import math
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

PARSER = argparse.ArgumentParser()
PARSER.add_argument("--dslc", required=True, type=Path)
PARSER.add_argument("--cxx", required=True)
PARSER.add_argument("--examples", required=True, type=Path)
PARSER.add_argument("--runtime-include", required=True, type=Path)
PARSER.add_argument("--runtime-library", required=True, type=Path)
ARGS = PARSER.parse_args()


def run(*args):
    return subprocess.run([str(arg) for arg in args], text=True, capture_output=True, timeout=60)


def compile_cpp(*args):
    libraries = [] if "-c" in args else [ARGS.runtime_library]
    return run(ARGS.cxx, "-I", ARGS.runtime_include, *args, *libraries)


def function(body, parameters="double a, double b, double c"):
    # Test fixture shorthand; the actual DSL file uses the public qualified API.
    names = "abs|sqrt|pow|exp|log|sin|cos|tan|min|max|floor|ceil|round"
    body = re.sub(rf"(?<![:\w])({names})\(", r"dsl_math::\1(", body)
    return f"#include <dsl_runtime/math.h>\ndouble compute({parameters}) {{\n{body}\n}}\n"


# Inputs deliberately distinguish precedence, associativity, and rounding.
POSITIVE = [
    ("average", "const double sum = a + b; return sum * 0.5;", (2.0, 4.0, 0.0), 3.0),
    ("precedence", "return a + b * c;", (2.0, 3.0, 4.0), 14.0),
    ("parentheses", "return (a + b) * c;", (2.0, 3.0, 4.0), 20.0),
    ("subtraction_left", "return a - b - c;", (10.0, 3.0, 2.0), 5.0),
    ("subtraction_grouped", "return a - (b - c);", (10.0, 3.0, 2.0), 9.0),
    ("division_left", "return a / b / c;", (24.0, 4.0, 2.0), 3.0),
    ("division_grouped", "return a / (b / c);", (24.0, 4.0, 2.0), 12.0),
    ("locals", "const double x = a + b; const double y = x / c; return x - y;", (2.0, 4.0, 3.0), 4.0),
    ("multiple_bindings", "double const x = a, y(x + b); return y;", (2.0, 4.0, 0.0), 6.0),
    ("parameter", "return a;", (-0.0, 4.0, 0.0), -0.0),
    ("parenthesized_parameter_return", "return ((a));", (-0.0, 4.0, 0.0), -0.0),
    ("parenthesized_local_return", "const double x = a; return ((x));", (-0.0, 4.0, 0.0), -0.0),
    ("negative_result", "return 0.0 - a;", (2.0, 0.0, 0.0), -2.0),
    ("rounding_left", "return a + b + c;", (1e16, -1e16, 1.0), 1.0),
    ("rounding_grouped", "return a + (b + c);", (1e16, -1e16, 1.0), 0.0),
    ("no_reassociation", "return (1e16 + a) - 1e16;", (1.0, 0.0, 0.0), 0.0),
    ("no_fma", "return a * b + c;", (1.0 + 2**-27, 1.0 - 2**-27, -1.0), 0.0),
    ("decimal_roundtrip", "return 1.0000000000000002;", (0.0, 0.0, 0.0), 1.0000000000000002),
    ("hex_subnormal", "return 0x0.0000000000001p-1022;", (0.0, 0.0, 0.0), float.fromhex("0x0.0000000000001p-1022")),
    ("max_double", "return 0x1.fffffffffffffp+1023;", (0.0, 0.0, 0.0), float.fromhex("0x1.fffffffffffffp+1023")),
    ("comments", "/* #define is only a comment */ const double x = (a); // hi\nreturn ((x + b));", (2.0, 4.0, 0.0), 6.0),
    ("generated_names", "const double v0 = a; const double v5 = v0 + b; return v5;", (2.0, 4.0, 0.0), 6.0),
]

POSITIVE += [
    ("unary_negative", "return -a;", (2.0, 0.0, 0.0), -2.0),
    ("unary_positive", "return +a;", (-0.0, 0.0, 0.0), -0.0),
    ("negative_zero_literal", "return -0.0;", (0.0, 0.0, 0.0), -0.0),
    ("negative_precedence", "return -a * b;", (2.0, 3.0, 0.0), -6.0),
    ("abs", "return abs(a);", (-2.5, 0.0, 0.0), 2.5),
    ("abs_negative_zero", "return abs(a);", (-0.0, 0.0, 0.0), 0.0),
    ("sqrt", "return sqrt(a);", (9.0, 0.0, 0.0), 3.0),
    ("pow", "return pow(a, b);", (2.0, 3.0, 0.0), 8.0),
    ("exp", "return exp(a);", (0.7, 0.0, 0.0), math.exp(0.7)),
    ("log", "return log(a);", (2.0, 0.0, 0.0), math.log(2.0)),
    ("sin", "return sin(a);", (0.7, 0.0, 0.0), math.sin(0.7)),
    ("cos", "return cos(a);", (0.7, 0.0, 0.0), math.cos(0.7)),
    ("tan", "return tan(a);", (0.7, 0.0, 0.0), math.tan(0.7)),
    ("min", "return min(a, b);", (-2.0, 4.0, 0.0), -2.0),
    ("max", "return max(a, b);", (-2.0, 4.0, 0.0), 4.0),
    ("floor", "return floor(a);", (-2.3, 0.0, 0.0), -3.0),
    ("ceil", "return ceil(a);", (-2.3, 0.0, 0.0), -2.0),
    ("round_negative_tie", "return round(a);", (-2.5, 0.0, 0.0), -3.0),
    ("round_positive_tie", "return round(a);", (2.5, 0.0, 0.0), 3.0),
    ("nested_math", "const double x = abs(a); return sqrt(pow(x, 2.0) + pow(b, 2.0));", (-3.0, 4.0, 0.0), 5.0),
    ("nested_call_arguments", "return pow(sqrt(a), abs(b));", (9.0, -2.0, 0.0), 9.0),
    ("conditional_true", "return a > b ? a : b;", (4.0, 2.0, 0.0), 4.0),
    ("conditional_false", "return a > b ? a : b;", (2.0, 4.0, 0.0), 4.0),
    ("less", "return a < b ? 1.0 : 0.0;", (1.0, 2.0, 0.0), 1.0),
    ("less_equal", "return a <= b ? 1.0 : 0.0;", (2.0, 2.0, 0.0), 1.0),
    ("greater_equal", "return a >= b ? 1.0 : 0.0;", (2.0, 2.0, 0.0), 1.0),
    ("equal", "return a == b ? 1.0 : 0.0;", (2.0, 2.0, 0.0), 1.0),
    ("not_equal", "return a != b ? 1.0 : 0.0;", (2.0, 3.0, 0.0), 1.0),
    ("bool_local", "const bool positive = a >= 0.0; return positive ? sqrt(a) : 0.0;", (9.0, 0.0, 0.0), 3.0),
    ("bool_reference", "const bool x = a > b; const bool y = x; return y ? a : b;", (2.0, 3.0, 0.0), 3.0),
    ("bool_literal", "const bool enabled = true; return enabled ? a : b;", (2.0, 3.0, 0.0), 2.0),
    ("bool_select", "const bool enabled = a < b ? true : false; return enabled ? a : b;", (2.0, 3.0, 0.0), 2.0),
    ("nested_select", "return a < 0.0 ? -a : a > b ? a : b;", (2.0, 3.0, 0.0), 3.0),
    ("select_in_call", "return sqrt(a < 0.0 ? -a : a);", (-9.0, 0.0, 0.0), 3.0),
    ("shared_outer_local", "const double x = a + b; return x > c ? x * x : x / 2.0;", (2.0, 4.0, 1.0), 36.0),
    ("branch_result_reused", "const double x = a < b ? a + 1.0 : b + 2.0; return x * x;", (2.0, 4.0, 0.0), 9.0),
]

REFERENCE_PRELUDE = """
#include <dsl_runtime/math.h>
#include <cmath>
namespace reference_math {
using std::abs; using std::sqrt; using std::pow; using std::exp; using std::log;
using std::sin; using std::cos; using std::tan;
using std::floor; using std::ceil; using std::round;
double min(double a, double b) { return std::fmin(a, b); }
double max(double a, double b) { return std::fmax(a, b); }
}
namespace reference_impl {
"""
REFERENCE_WRAPPER = """
}
double reference(double a, double b, double c) {
    return reference_impl::compute(a, b, c);
}
"""

# Every rejection also checks source coordinates and preservation of old output.
NEGATIVE = [
    ("empty", "", "expected exactly one"),
    ("wrong_name", "double other() { return 1.0; }", "named compute"),
    ("prototype", "double compute(double a);", "must have a definition"),
    ("global", "const double x = 1.0; double compute() { return x; }", "only global DSL function declarations"),
    ("extra_semicolon", "double compute() { return 1.0; };", "only global DSL function declarations"),
    ("int_return", "int compute() { return 1; }", "unsupported token 'int'"),
    ("const_return", "const double compute() { return 1.0; }", "unqualified double return"),
    ("float_parameter", "double compute(float a) { return a; }", "unsupported token 'float'"),
    ("const_parameter", "double compute(const double a) { return a; }", "parameters must"),
    ("reference_parameter", "double compute(double& a) { return a; }", "unsupported token '&'"),
    ("pointer_parameter", "double compute(double* a) { return 1.0; }", "parameters must"),
    ("default_parameter", "double compute(double a = 1.0) { return a; }", "default arguments"),
    ("variadic", "double compute(double a, ...) { return a; }", "unsupported token '...'"),
    ("mutable_local", function("double x = a; return x;"), "initialized const double"),
    ("uninitialized", function("const double x; return a;"), "initialization"),
    ("self_reference", function("const double x = x; return x;"), "already initialized"),
    ("assign_parameter", function("a = b; return a;"), "unsupported statement"),
    ("assign_in_return", function("return a = b;"), "only binary"),
    ("assign_local", function("const double x = a; x = b; return x;"), "const-qualified"),
    ("compound_assignment", function("a += b; return a;"), "unsupported token '+='"),
    ("increment", function("return a++;"), "unsupported token '++'"),
    ("integer_literal", function("return a + 1;"), "implicit conversions"),
    ("float_literal", function("return 1.0f;"), "implicit conversions"),
    ("long_double_literal", function("return 1.0L;"), "implicit conversions"),
    ("functional_cast", function("return double(a);"), "CXXFunctionalCastExpr"),
    ("c_cast", function("return (double)a;"), "CStyleCastExpr"),
    ("static_cast", function("return static_cast<double>(a);"), "unsupported token 'static_cast'"),
    ("comma", function("return (a, b);"), "comma operator has no effect"),
    ("comparison_return", function("return a < b;"), "implicit conversions"),
    ("numeric_condition", function("return a ? b : c;"), "implicit conversions"),
    ("if", function("if (a) return b; return c;"), "unsupported token 'if'"),
    ("while", function("while (a) {} return b;"), "unsupported token 'while'"),
    ("for", function("for (;;) {} return a;"), "unsupported token 'for'"),
    ("block", function("{ const double x = a; } return a;"), "CompoundStmt"),
    ("empty_statement", function("; return a;"), "NullStmt"),
    ("expression_statement", function("a + b; return a;"), "expression result unused"),
    ("missing_return", function("const double x = a;"), "does not return a value"),
    ("early_return", function("return a; return b;"), "only as the final"),
    ("after_return", function("return a; const double x = b;"), "must end"),
    ("bare_return", function("return;"), "non-void"),
    ("braced_init", function("const double x{a}; return x;"), "InitListExpr"),
    ("braced_return", function("return {a};"), "braces around scalar initializer"),
    ("auto", function("const auto x = a; return x;"), "unsupported token 'auto'"),
    ("volatile", function("const volatile double x = a; return x;"), "unsupported token 'volatile'"),
    ("static_local", function("static const double x = 1.0; return x;"), "unsupported token 'static'"),
    ("constexpr", "constexpr double compute() { return 1.0; }", "unsupported token 'constexpr'"),
    ("inline", "inline double compute() { return 1.0; }", "unsupported token 'inline'"),
    ("noexcept", "double compute() noexcept { return 1.0; }", "unsupported token 'noexcept'"),
    ("namespace", "namespace n { double compute() { return 1.0; } }", "unsupported token 'namespace'"),
    ("template", "template<class T> double compute() { return 1.0; }", "unsupported token 'template'"),
    ("using", "using D = double; double compute() { return 1.0; }", "unsupported token 'using'"),
    ("attribute", "[[nodiscard]] double compute() { return 1.0; }", "unsupported token '['"),
    ("gnu_attribute", "__attribute__((unused)) double compute() { return 1.0; }", "unsupported token '__attribute__'"),
    ("include", '#include "does-not-exist.h"\n' + function("return a;"), "only #include <dsl_runtime/math.h>"),
    ("macro", "#define X 1.0\n" + function("return X;"), "preprocessor directives"),
    ("inactive_code", "#if 0\nclass X {};\n#endif\n" + function("return a;"), "preprocessor directives"),
    ("null_directive", "#\n" + function("return a;"), "preprocessor directives"),
    ("digraph_directive", "%:define X 1.0\n" + function("return X;"), "preprocessor directives"),
    ("line_directive", '#line 400 "fake.cpp"\n' + function("return a;"), "preprocessor directives"),
    ("pragma", "#pragma STDC FP_CONTRACT ON\n" + function("return a;"), "preprocessor directives"),
    ("pragma_operator", '_Pragma("STDC FP_CONTRACT ON")\n' + function("return a;"), "pragmas"),
    ("builtin_macro", function("return __DBL_MAX__;"), "macro expansions"),
    ("builtin_line", function("return __LINE__;"), "macro expansions"),
    ("undefined_name", function("return missing;"), "undeclared identifier"),
    ("syntax_error", function("return a + ;"), "expected expression"),
    ("overflow", function("return 1e9999;"), "floating-point constant too large"),
]

NEGATIVE += [
    ("unknown_math", function("return cot(a);"), "undeclared identifier"),
    ("sqrt_arity", function("return sqrt(a, b);"), "too many arguments"),
    ("pow_arity", function("return pow(a);"), "too few arguments"),
    ("no_arguments", function("return sqrt();"), "too few arguments"),
    ("math_integer", function("return pow(a, 2);"), "implicit conversions"),
    ("math_float", function("return sqrt(1.0f);"), "implicit conversions"),
    ("math_bool", function("return abs(true);"), "implicit conversions"),
    ("integer_comparison", function("return a < 1 ? a : b;"), "implicit conversions"),
    ("mixed_branches", function("return a > b ? a : 1;"), "implicit conversions"),
    ("mixed_bool_branches", function("return a > b ? a : false;"), "implicit conversions"),
    ("bool_from_double", function("const bool x = a; return x ? b : c;"), "implicit conversions"),
    ("double_from_bool", function("const double x = a > b; return x;"), "implicit conversions"),
    ("bool_arithmetic", function("return true + false;"), "implicit conversions"),
    ("bool_parameter", "double compute(bool a) { return 1.0; }", "parameters must"),
    ("bool_return", "bool compute(double a) { return true; }", "unqualified double return"),
    ("mutable_bool", function("bool x = true; return x ? a : b;"), "initialized const double or const bool"),
    ("logical_and", function("return (a < b && b < c) ? a : c;"), "unsupported token '&&'"),
    ("logical_or", function("return (a < b || b < c) ? a : c;"), "unsupported token '||'"),
    ("builtin_redeclare", "double sqrt(double a);\n" + function("return sqrt(a);"), "must have a definition"),
    ("local_function", function("double helper(double x); return helper(a);"), "locals must"),
    ("namespace_call", function("return std::sqrt(a);"), "undeclared"),
]

NEGATIVE += [
    ("undefined_helper", "double helper(double); double compute(double a) { return helper(a); }", "must have a definition"),
    ("undeclared_forward", "double compute(double a) { return helper(a); } double helper(double a) { return a; }", "undeclared identifier"),
    ("overloaded_helper", "double helper(double a) { return a; } double helper(double a, double b) { return a + b; } double compute(double a) { return helper(a); }", "overloading"),
    ("duplicate_definition", "double compute() { return 1.0; } double compute() { return 2.0; }", "redefinition"),
    ("invalid_unused_helper", "double helper(double a) { double x = a; return x; } double compute() { return 1.0; }", "locals must"),
    ("helper_bool_return", "bool helper(double a) { return true; } double compute() { return 1.0; }", "unqualified double return"),
    ("helper_arity", "double helper(double a) { return a; } double compute() { return helper(); }", "no matching function"),
    ("helper_integer_argument", "double helper(double a) { return a; } double compute() { return helper(1); }", "implicit conversions"),
    ("missing_runtime_include", "double compute(double a) { return dsl_math::sqrt(a); }", "undeclared identifier"),
    ("runtime_redefinition", "#include <dsl_runtime/math.h>\ndouble dsl_math::sqrt(double a) { return a; } double compute(double a) { return dsl_math::sqrt(a); }", "only global DSL function"),
    ("other_header", "#include <cmath>\ndouble compute() { return 1.0; }", "only #include <dsl_runtime/math.h>"),
]


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dsl-tests-")
        self.root = Path(self.temp.name)
        self.source = self.root / "input.cpp"
        self.output = self.root / "generated.cpp"

    def tearDown(self):
        self.temp.cleanup()

    def compile_dsl(self, source):
        self.source.write_text(source)
        return run(ARGS.dslc, self.source, "--dump-ir", "-o", self.output)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_acceptance_example(self):
        self.assert_ok(self.compile_dsl((ARGS.examples / "average.dsl.cpp").read_text()))
        ir = run(ARGS.dslc, self.source, "--dump-ir", "-o", self.output)
        self.assert_ok(ir)
        self.assertEqual(ir.stdout, """computation_ir v1
type !0 = ieee754.binary64
type !1 = bool
type !2 = i32
constant #c0 : !0 = bits 0x3fe0000000000000
func @compute ( %0: !0 %1: !0 ) -> ( !0 ) {
  value %0 : !0
  value %1 : !0
  value %2 : !0
  value %3 : !0
  value %4 : !0
  value %5 : !0
  %2 = add %0, %1
  %3 = identity %2
  %4 = constant #c0
  %5 = mul %3, %4
  return_success %5
}
""")
        self.assertNotIn("sum", self.output.read_text())
        executable = self.root / "example"
        self.assert_ok(compile_cpp("-std=c++23", "-O2", "-fno-fast-math", "-ffp-contract=off",
                           self.output, ARGS.examples / "driver.cpp", "-o", executable))
        result = run(executable)
        self.assert_ok(result)
        self.assertEqual(result.stdout, "3\n")

    def test_generated_execution_and_bitwise_reference(self):
        for name, body, arguments, expected in POSITIVE:
            with self.subTest(name=name):
                source = function(body)
                result = self.compile_dsl(source)
                self.assert_ok(result)
                reference = self.root / "reference.cpp"
                reference.write_text(REFERENCE_PRELUDE + source.replace("dsl_math::", "reference_math::") + REFERENCE_WRAPPER)
                driver = self.root / "driver.cpp"
                args = ", ".join(value.hex() for value in arguments)
                driver.write_text(f"""
#include <cstdint>
#include <cstring>
#include <iostream>
double compute(double, double, double);
double reference(double, double, double);
std::uint64_t bits(double value) {{
    std::uint64_t result;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}}
int main() {{
    const double actual = compute({args});
    const double original = reference({args});
    const double expected = {expected.hex()};
    if (bits(actual) != bits(original) || bits(actual) != bits(expected)) {{
        std::cerr << std::hexfloat << actual << " != " << original << " or " << expected;
        return 1;
    }}
}}
""")
                executable = self.root / "program"
                self.assert_ok(compile_cpp("-std=c++23", "-O2", "-fno-fast-math", "-ffp-contract=off",
                                   self.output, reference, driver, "-o", executable))
                self.assert_ok(run(executable))

    def test_dsl_function_calls(self):
        modules = [
            ("helper", "double square(double x) { return x * x; } double compute(double a) { return square(a); }", "3.0", "9.0", "square"),
            ("forward", "double square(double); double compute(double a) { return square(a); } double square(double x) { return x * x; }", "3.0", "9.0", "square"),
            ("entry_prototype", "double compute(double); double compute(double a) { return a; }", "3.0", "3.0", "compute"),
            ("recursive_helper", "double fact(double n) { return n <= 1.0 ? 1.0 : n * fact(n - 1.0); } double compute(double a) { return fact(a); }", "5.0", "120.0", "fact"),
            ("recursive_entry", "double compute(double a) { return a <= 1.0 ? 1.0 : a * compute(a - 1.0); }", "5.0", "120.0", "compute"),
            ("mutual", "double odd(double); double even(double n) { return n <= 0.0 ? 1.0 : odd(n - 1.0); } double odd(double n) { return n <= 0.0 ? 0.0 : even(n - 1.0); } double compute(double a) { return even(a); }", "6.0", "1.0", "odd"),
            ("name_collision", "double v0(double x) { return x + 1.0; } double compute(double v0) { return ::v0(v0); }", "2.0", "3.0", "v0"),
            ("runtime_and_dsl", "#include <dsl_runtime/math.h>\ndouble sqrt(double a) { return a + 1.0; } double compute(double a) { return sqrt(a) + dsl_math::sqrt(a); }", "9.0", "13.0", "sqrt"),
            ("chained", "double square(double x) { return x * x; } double twice(double x) { return square(x) + square(x); } double compute(double a) { return twice(a); }", "3.0", "18.0", "twice"),
        ]
        for name, source, argument, expected, callee in modules:
            with self.subTest(name=name):
                result = self.compile_dsl(source)
                self.assert_ok(result)
                self.assertIn(f"func @{callee}", result.stdout)
                driver = self.root / "module_driver.cpp"
                driver.write_text(f"double compute(double); int main() {{ return compute({argument}) == {expected} ? 0 : 1; }}")
                executable = self.root / "module"
                self.assert_ok(compile_cpp("-std=c++23", "-O2", "-fno-fast-math", "-ffp-contract=off",
                                           self.output, driver, "-o", executable))
                self.assert_ok(run(executable))

    def test_external_c_and_cpp_linkage(self):
        headers = self.root / "headers"
        headers.mkdir()
        c_header = headers / "native.h"
        c_header.write_text("""#ifndef NATIVE_H
#define NATIVE_H
#ifdef __cplusplus
extern "C" {
#endif
double native_scale(double);
#ifdef __cplusplus
}
#endif
#endif
""")
        cpp_header = headers / "api.h"
        cpp_header.write_text('#pragma once\n#include "native.h"\nnamespace vendor::math { double offset(double, double); }\n')
        self.source.write_text("""#include <api.h>
double helper(double x) { return vendor::math::offset(native_scale(x), 1.0); }
double compute(double x) { return helper(x); }
""")
        result = run(ARGS.dslc, self.source, "--extern-header", cpp_header,
                     "--extern-header", c_header, "--dump-ir", "-o", self.output)
        self.assert_ok(result)
        self.assertIn("external @external1 // vendor::math::offset", result.stdout)
        self.assertIn("external_call %1, %2 @external1", result.stdout)
        self.assertIn("external @external0 // native_scale", result.stdout)
        generated = self.output.read_text()
        self.assertIn("::vendor::math::offset(", generated)
        self.assertIn(str(cpp_header), generated)
        c_source = self.root / "native.c"
        c_source.write_text('#include "headers/native.h"\ndouble native_scale(double x) { return x * 2.0; }\n')
        c_object = self.root / "native.o"
        self.assert_ok(run(ARGS.cxx, "-x", "c", "-std=c17", "-c", c_source, "-o", c_object))
        cpp_source = self.root / "api.cpp"
        cpp_source.write_text('#include "headers/api.h"\nnamespace vendor::math { double offset(double x, double y) { for (int i = 0; i < 3; ++i) x += y; return x; } }\n')
        cpp_object = self.root / "api.o"
        self.assert_ok(run(ARGS.cxx, "-std=c++23", "-c", cpp_source, "-o", cpp_object))
        driver = self.root / "driver.cpp"
        driver.write_text('double compute(double); int main() { return compute(4.0) == 11.0 ? 0 : 1; }')
        executable = self.root / "external"
        # Output can move without changing how its physical API headers resolve.
        relocated = self.root / "out"
        relocated.mkdir()
        moved = relocated / "generated.cpp"
        moved.write_text(generated)
        self.assert_ok(compile_cpp("-std=c++23", moved, driver, c_object, cpp_object, "-o", executable))
        self.assert_ok(run(executable))
        missing = compile_cpp("-std=c++23", moved, driver, "-o", executable)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("undefined reference", missing.stderr)

    def test_external_effects_and_lazy_calls(self):
        header = self.root / "effects.h"
        header.write_text('#pragma once\nnamespace effects { double next(); double pair(double, double); double bad(); }\n#define v0 99\n#define dsl_function_0 forbidden\n')
        implementation = self.root / "effects.cpp"
        implementation.write_text("""#include "effects.h"
namespace effects {
int count = 0;
double next() { return ++count; }
double pair(double a, double b) { return a * 10.0 + b; }
double bad() { count += 100; return -1.0; }
}
double compute(double);
int main() {
    if (compute(1.0) != 12.0 || effects::count != 2) return 1;
    if (compute(-1.0) != 34.0 || effects::count != 4) return 2;
}
""")
        self.source.write_text("""#include "effects.h"
double helper() { return effects::pair(effects::next(), effects::next()); }
double compute(double x) {
    return x > 0.0 ? helper() : (x < 0.0 ? helper() : effects::bad());
}
""")
        self.assert_ok(run(ARGS.dslc, self.source, "--extern-header", header, "-o", self.output))
        executable = self.root / "effects"
        self.assert_ok(compile_cpp("-std=c++23", "-O2", self.output, implementation, "-o", executable))
        self.assert_ok(run(executable))

    def test_external_rejections(self):
        header = self.root / "api.h"
        cases = [
            ("int return", "int api(double);", "return api(x);"),
            ("pointer", "double api(double*);", "return x;"),
            ("reference", "double api(double&);", "return x;"),
            ("bool", "double api(bool);", "return x;"),
            ("default", "double api(double = 1.0);", "return x;"),
            ("variadic", "double api(double, ...);", "return x;"),
            ("definition", "double api(double x) { return x; }", "return x;"),
            ("static", "static double api(double);", "return x;"),
            ("overload", "double api(double); double api(double, double);", "return x;"),
            ("anonymous", "namespace { double api(double); }", "return x;"),
            ("template", "template<class T> double api(T);", "return x;"),
            ("variable", "double value;", "return x;"),
            ("member", "struct A { static double api(double); };", "return x;"),
            ("runtime namespace", "namespace dsl_math { double api(double); }", "return x;"),
            ("entry collision", "double compute(double);", "return x;"),
            ("helper collision", "double dsl_function_0(double);", "return x;"),
            ("namespace collision", "namespace dsl_function_0 { double api(double); }", "return x;"),
            ("inline collision", "inline namespace vendor { double dsl_function_0(double); }", "return x;"),
            ("argument conversion", "double api(double);", "return api(1);"),
            ("wrong arity", "double api(double);", "return api(x, x);"),
            ("macro call", "#define api(x) (x)\n", "return api(x);"),
            ("unauthorized dependency", '#include "dependency.h"', "return x;"),
        ]
        (self.root / "dependency.h").write_text("double hidden(double);")
        for label, declaration, body in cases:
            with self.subTest(label=label):
                header.write_text(declaration)
                self.source.write_text('#include "api.h"\ndouble compute(double x) { ' + body + ' }')
                self.output.write_text("preserved")
                result = run(ARGS.dslc, self.source, "--extern-header", header,
                             "--dump-ir", "-o", self.output)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertEqual(self.output.read_text(), "preserved")
                self.assertRegex(result.stderr, r":\d+:\d+: error:")
        header.write_text("double api(double);")
        for source in [
            '#include "api.h"\ndouble api(double); double compute(double x) { return api(x); }',
            '#include "api.h"\ndouble api(double x) { return x; } double compute(double x) { return api(x); }',
            'double api(double);\n#include "api.h"\ndouble compute(double x) { return api(x); }',
        ]:
            self.source.write_text(source)
            result = run(ARGS.dslc, self.source, "--extern-header", header, "-o", self.output)
            self.assertNotEqual(result.returncode, 0)
        self.source.write_text('#include "api.h"\ndouble compute(double x) { return api(x); }')
        self.assertNotEqual(run(ARGS.dslc, self.source, "-o", self.output).returncode, 0)
        approved = self.root / "approved"
        approved.mkdir()
        trusted = approved / "api.h"
        trusted.write_text("double api(double);")
        shadow = run(ARGS.dslc, self.source, "--extern-header", trusted, "-o", self.output)
        self.assertNotEqual(shadow.returncode, 0)
        self.assertIn("include must resolve", shadow.stderr)
        original = header.read_text()
        self.assertNotEqual(run(ARGS.dslc, self.source, "--extern-header", header, "-o", header).returncode, 0)
        self.assertEqual(header.read_text(), original)
        self.assertNotEqual(run(ARGS.dslc, self.source, "-o", self.output, "--extern-header").returncode, 0)
        self.assertNotEqual(run(ARGS.dslc, self.source, "-o", self.output, "--extern-header", self.root / "missing.h").returncode, 0)

    def test_runtime_is_a_linked_library(self):
        # The public library is callable from ordinary C++ without invoking dslc.
        ordinary = self.root / "ordinary.cpp"
        ordinary.write_text("""
#include <dsl_runtime/math.h>
int main() { return dsl_math::sqrt(9.0) == 3.0 && dsl_math::pow(2.0, 3.0) == 8.0 ? 0 : 1; }
""")
        executable = self.root / "ordinary"
        self.assert_ok(compile_cpp("-std=c++23", ordinary, "-o", executable))
        self.assert_ok(run(executable))
        self.assert_ok(self.compile_dsl(function("return sqrt(a);", "double a")))
        generated = self.output.read_text()
        self.assertIn("dsl_math::sqrt", generated)
        self.assertNotIn("std::sqrt", generated)
        driver = self.root / "link_driver.cpp"
        driver.write_text("double compute(double); int main() { return compute(9.0) == 3.0 ? 0 : 1; }")
        missing_library = run(ARGS.cxx, "-std=c++23", "-I", ARGS.runtime_include,
                              self.output, driver, "-o", self.root / "unlinked")
        self.assertNotEqual(missing_library.returncode, 0)
        self.assertIn("undefined", missing_library.stderr)

    def test_header_identity_and_quoted_include(self):
        quoted = '#include "dsl_runtime/math.h"\ndouble compute(double a) { return dsl_math::sqrt(a); }'
        self.assert_ok(self.compile_dsl(quoted))
        shadow = self.root / "dsl_runtime"
        shadow.mkdir()
        (shadow / "math.h").write_text("namespace dsl_math { double sqrt(double); }\n")
        self.output.write_text("existing output\n")
        result = self.compile_dsl(quoted)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("include must resolve to the configured runtime header", result.stderr)
        self.assertEqual(self.output.read_text(), "existing output\n")

    def test_math_example(self):
        result = self.compile_dsl((ARGS.examples / "math.dsl.cpp").read_text())
        self.assert_ok(result)
        self.assertIn(" = sqrt %", result.stdout)
        self.assertIn(" = if %", result.stdout)
        executable = self.root / "math-example"
        self.assert_ok(compile_cpp("-std=c++23", "-O2", "-fno-fast-math", "-ffp-contract=off",
                           self.output, ARGS.examples / "math_driver.cpp", "-o", executable))
        result = run(executable)
        self.assert_ok(result)
        self.assertEqual(result.stdout, "5\n")

    def test_lazy_selection(self):
        cases = [
            ("guard_sqrt", "return a >= 0.0 ? sqrt(a) : b;", "-1.0, 7.0, 0.0"),
            ("guard_log", "return a > 0.0 ? b : log(-a);", "1.0, 7.0, 0.0"),
            ("guard_division", "return a > 0.0 ? b : 1.0 / c;", "1.0, 7.0, 0.0"),
            ("nested_guard", "return a < 0.0 ? b : c > 0.0 ? sqrt(c) : log(c);", "-1.0, 7.0, -1.0"),
        ]
        for name, body, arguments in cases:
            with self.subTest(name=name):
                result = self.compile_dsl(function(body))
                self.assert_ok(result)
                self.assertIn(" = if %", result.stdout)
                self.assertIn("yield %", result.stdout)
                driver = self.root / "lazy.cpp"
                driver.write_text(f"""
#include <cerrno>
#include <cfenv>
#include <iostream>
double compute(double, double, double);
int main() {{
    std::feclearexcept(FE_ALL_EXCEPT);
    errno = 0;
    const double value = compute({arguments});
    const int errors = std::fetestexcept(FE_INVALID | FE_DIVBYZERO);
    if (value != 7.0 || errors != 0 || errno != 0) {{
        std::cerr << "value=" << value << " exceptions=" << errors << " errno=" << errno;
        return 1;
    }}
}}
""")
                executable = self.root / "lazy"
                self.assert_ok(compile_cpp("-std=c++23", "-O2", "-fno-fast-math", "-ffp-contract=off",
                                   self.output, driver, "-o", executable))
                self.assert_ok(run(executable))

    def test_math_nan_and_infinity(self):
        body = """
return c == 0.0 ? sqrt(a)
     : c == 1.0 ? log(a)
     : c == 2.0 ? min(a, b)
     : c == 3.0 ? max(a, b)
     : c == 4.0 ? (a == b ? 1.0 : 0.0)
     : c == 5.0 ? (a != b ? 1.0 : 0.0)
     : pow(a, b);
"""
        self.assert_ok(self.compile_dsl(function(body)))
        driver = self.root / "special.cpp"
        driver.write_text("""
#include <cmath>
#include <limits>
double compute(double, double, double);
int main() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!std::isnan(compute(-1.0, 0.0, 0.0))) return 1;
    const double logarithm = compute(0.0, 0.0, 1.0);
    if (!std::isinf(logarithm) || !std::signbit(logarithm)) return 2;
    if (compute(nan, 2.0, 2.0) != 2.0 || compute(2.0, nan, 2.0) != 2.0) return 3;
    if (compute(nan, 2.0, 3.0) != 2.0 || compute(2.0, nan, 3.0) != 2.0) return 4;
    if (!std::isnan(compute(nan, nan, 2.0))) return 5;
    if (compute(nan, nan, 4.0) != 0.0 || compute(nan, nan, 5.0) != 1.0) return 6;
    if (!std::isnan(compute(-1.0, 0.5, 6.0))) return 7;
}
""")
        executable = self.root / "special"
        self.assert_ok(compile_cpp("-std=c++23", "-O2", "-fno-fast-math", "-ffp-contract=off",
                           self.output, driver, "-o", executable))
        self.assert_ok(run(executable))

    def test_zero_and_unnamed_parameters(self):
        for source in [function("return 0.5;", ""), function("return 0.5;", "double")]:
            with self.subTest(source=source):
                self.assert_ok(self.compile_dsl(source))
                self.assert_ok(compile_cpp("-std=c++23", "-c", self.output, "-o", self.root / "out.o"))

    def test_rejections(self):
        for name, source, reason in NEGATIVE:
            with self.subTest(name=name):
                self.output.write_text("existing output\n")
                result = self.compile_dsl(source)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(reason, result.stderr)
                self.assertRegex(result.stderr, re.escape(str(self.source)) + r":\d+:\d+: error:")
                self.assertEqual(result.stdout, "")
                self.assertEqual(self.output.read_text(), "existing output\n")
                self.assertEqual(list(self.root.glob("*.tmp-*")), [])

    def test_precise_original_location(self):
        result = self.compile_dsl("double compute(double a) {\n  return a + 1;\n}\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{self.source}:2:14: error: DSL: implicit conversions", result.stderr)

    def test_cli_and_io_errors(self):
        self.assert_ok(run(ARGS.dslc, "--help"))
        version = run(ARGS.dslc, "--version")
        self.assert_ok(version)
        self.assertIn("20.1.8", version.stdout)
        for arguments in [[], ["--unknown"], [self.source], [self.source, "-o"],
                          [self.source, "other.cpp", "-o", self.output],
                          [self.source, "-o", self.output, "-o", self.output]]:
            with self.subTest(arguments=arguments):
                self.assertNotEqual(run(ARGS.dslc, *arguments).returncode, 0)
        missing = run(ARGS.dslc, self.source, "-o", self.output)
        self.assertIn("cannot read", missing.stderr)
        self.assertFalse(self.output.exists())
        self.source.write_text(function("return a;"))
        before = self.source.read_text()
        for target in [self.source, self.root / "alias.cpp"]:
            if target != self.source:
                target.symlink_to(self.source)
            result = run(ARGS.dslc, self.source, "-o", target)
            self.assertIn("must be different files", result.stderr)
            self.assertEqual(self.source.read_text(), before)
        result = run(ARGS.dslc, self.source, "-o", self.root / "missing" / "output.cpp")
        self.assertIn("cannot create output", result.stderr)
        result = run(ARGS.dslc, self.source, "-o", self.root)
        self.assertIn("cannot save output", result.stderr)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.root.parent.glob(self.root.name + ".tmp-*")), [])


if __name__ == "__main__":
    print(f"Testing {len(POSITIVE)} execution cases and {len(NEGATIVE)} rejected programs", flush=True)
    unittest.main(argv=[__file__], verbosity=2)
