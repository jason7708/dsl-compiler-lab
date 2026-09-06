#!/usr/bin/env python3
"""Compile real object libraries and exercise their binding/evaluation boundary."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--dslc', required=True, type=Path)
parser.add_argument('--cxx', required=True)
parser.add_argument('--examples', required=True, type=Path)
parser.add_argument('--runtime-include', required=True, type=Path)
parser.add_argument('--runtime-library', required=True, type=Path)
args = parser.parse_args()


def run(*command):
    return subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=60)


class Objects(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dsl-objects-')
        self.root = Path(self.temp.name)
        self.api = self.root / 'api.h'
        self.api.write_text('''#pragma once
struct ext_event { int instrument_id; double reference_price; bool enabled; };
double get_bid(int);
double get_ask(int);
''')
        self.source = self.root / 'input.dsl.cpp'
        self.output = self.root / 'generated.h'

    def tearDown(self):
        self.temp.cleanup()

    def compile(self, source, *options):
        self.source.write_text(source)
        return run(args.dslc, self.source, '--emit-objects', '--extern-header', self.api,
                   '--dump-ir', '-o', self.output, *options)

    def ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def execute(self, source, *extra):
        driver = self.root / 'driver.cpp'
        driver.write_text(source)
        executable = self.root / 'driver'
        self.ok(run(args.cxx, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-fno-fast-math', '-ffp-contract=off', '-I', args.runtime_include,
                    driver, *extra, args.runtime_library, '-o', executable))
        self.ok(run(executable))

    def mid(self):
        return '''#include "api.h"
double mid_price(ext_event event) {
    double bid = get_bid(event.instrument_id);
    double ask = get_ask(event.instrument_id);
    double mid = (bid + ask) / 2;
    return mid;
}
'''

    def test_binding_and_pure_evaluation(self):
        result = self.compile(self.mid(), '--context-function', 'get_bid', '--context-function', 'get_ask')
        self.ok(result)
        self.assertIn('context bid <- @get_bid', result.stdout)
        self.assertIn('context ask <- @get_ask', result.stdout)
        self.assertIn('using mid_price_event = ::ext_event;', self.output.read_text())
        self.execute('''#include "generated.h"
#include <type_traits>
int reads = 0;
double get_bid(int id) { if (id != 7 || reads != 0) return -1000.0; ++reads; return 100.0; }
double get_ask(int id) { if (id != 7 || reads != 1) return -1000.0; ++reads; return 104.0; }
int main() {
    static_assert(std::is_empty_v<mid_price>);
    static_assert(std::is_empty_v<mid_price_state>);
    static_assert(std::is_same_v<mid_price_output, mid_price::contract_t::output>);
    static_assert(sizeof(mid_price_context) == 2 * sizeof(double));
    const ext_event event{7, 0.0, true};
    const auto ctx = prepare_mid_price_context(event);
    if (ctx.bid != 100.0 || ctx.ask != 104.0 || reads != 2) return 1;
    dsl_runtime::unit<mid_price> unit;
    auto first = unit.on_event(ctx, event);
    auto second = unit.on_event(ctx, event);
    if (!first || !second || first->result != 102.0 || second->result != 102.0 || reads != 2) return 2;
    const mid_price op;
    auto manual = op({10.0, 20.0}, {}, event);
    return manual && manual->result == 15.0 && reads == 2 ? 0 : 3;
}
''')

    def test_manual_context_does_not_need_provider_implementation(self):
        self.ok(self.compile(self.mid(), '--context-function', 'get_bid', '--context-function', 'get_ask'))
        self.execute('''#include "generated.h"
int main() {
    auto result = mid_price{}({10.0, 14.0}, {}, {0, 0.0, true});
    return result && result->result == 12.0 ? 0 : 1;
}
''')

    def test_source_library_composition_and_repeated_reads(self):
        library = self.root / 'pricing.dsl.h'
        library.write_text('#pragma once\n' + self.mid())
        result = self.compile('''#include "pricing.dsl.h"
double difference(ext_event event) {
    double first = mid_price(event);
    double second = mid_price(event);
    return first + second - event.reference_price;
}
''', '--dsl-library', library, '--context-function', 'get_bid', '--context-function', 'get_ask')
        self.ok(result)
        self.assertNotIn('#include "' + str(library), self.output.read_text())
        self.assertNotIn('call @mid_price', result.stdout)
        self.execute('''#include "generated.h"
int reads = 0;
double get_bid(int id) { return id + 10.0 * ++reads; }
double get_ask(int id) { return id + 10.0 * ++reads; }
int main() {
    const ext_event event{2, 4.0, true};
    auto ctx = prepare_difference_context(event);
    if (reads != 4 || sizeof(ctx) != 4 * sizeof(double)) return 1;
    auto result = difference{}(ctx, {}, event);
    // Reads yield 12, 22, 32, 42: mids 17 + 37 - 4.
    return result && result->result == 50.0 && reads == 4 ? 0 : 2;
}
''')

    def test_scalar_helpers_and_lazy_math(self):
        result = self.compile('''#include "api.h"
#include <dsl_runtime/math.h>
double square(double x) { return x * x; }
double constant() { return 2; }
double named(double, double arg_0) { return arg_0; }
double root(ext_event event) {
    return event.enabled ? (event.instrument_id == 7 ? square(event.reference_price) : 0.0) : dsl_math::sqrt(event.reference_price);
}
''')
        self.ok(result)
        self.execute('''#include "generated.h"
#include <cfenv>
#include <cmath>
int main() {
    const auto ctx = prepare_root_context({7, -4.0, true});
    std::feclearexcept(FE_ALL_EXCEPT);
    auto value = root{}(ctx, {}, {7, -4.0, true});
    if (!value || value->result != 16.0 || std::fetestexcept(FE_INVALID)) return 1;
    auto c = constant{}({}, {}, {});
    auto n = named{}({}, {}, {5.0, 8.0});
    return c && c->result == 2.0 && n && n->result == 8.0 ? 0 : 2;
}
''')

    def test_unit_success_failure_and_instance_state(self):
        self.execute('''#include <dsl_runtime/operation.h>
struct Context { double factor; };
struct State { double total; };
struct Event { double value; bool valid; };
enum class Error { invalid };
struct Intent {};
struct Accumulate {
    using contract_t = dsl_runtime::contract<Context, State, Event, double, Error, Intent>;
    contract_t::response operator()(const Context& ctx, const State& state, const Event& event) const {
        if (!event.valid) return std::unexpected(Error::invalid);
        const auto next = state.total + event.value * ctx.factor;
        return contract_t::output{next, {}, {next}};
    }
};
int main() {
    dsl_runtime::unit<Accumulate> a(State{10.0}), b(State{100.0});
    auto success = a.on_event({2.0}, {3.0, true});
    if (!success || success->result != 16.0 || a.state().total != 16.0) return 1;
    auto failure = a.on_event({2.0}, {9.0, false});
    if (failure || failure.error() != Error::invalid || a.state().total != 16.0) return 2;
    if (b.state().total != 100.0) return 3;
    return b.on_event({1.0}, {2.0, true})->new_state.total == 102.0 ? 0 : 4;
}
''')

    def test_generated_header_multiple_translation_units(self):
        self.ok(self.compile('double square(double x) { return x * x; }'))
        other = self.root / 'other.cpp'
        other.write_text('#include "generated.h"\ndouble other() { return square{}({}, {}, {3.0})->result; }')
        self.execute('''#include "generated.h"
#include "generated.h"
double other();
int main() { return other() == 9.0 && square{}({}, {}, {2.0})->result == 4.0 ? 0 : 1; }
''', other)

    def test_rejections(self):
        cases = [
            ('unregistered provider', self.mid(), (), 'context-function'),
            ('unknown provider', 'double f() { return 1.0; }', ('--context-function', 'missing'), 'no imported declaration'),
            ('conditional read', '#include "api.h"\ndouble f(ext_event e) { return e.enabled ? get_bid(e.instrument_id) : 0.0; }', ('--context-function', 'get_bid'), 'conditional'),
            ('conditional helper', '#include "api.h"\ndouble helper(ext_event e) { return get_bid(e.instrument_id); } double f(ext_event e) { return e.enabled ? helper(e) : 0.0; }', ('--context-function', 'get_bid'), 'conditional'),
            ('recursion', 'double f(double x) { return x <= 0.0 ? 0.0 : f(x - 1.0); }', (), 'recursive'),
            ('event assignment', '#include "api.h"\ndouble f(ext_event e) { e.instrument_id = 2; return 1.0; }', (), 'read-only'),
            ('bad field', '#include "api.h"\ndouble f(ext_event e) { return get_bid(e.id); }', ('--context-function', 'get_bid'), 'no member'),
            ('record and scalar params', '#include "api.h"\ndouble f(ext_event e, double x) { return x; }', (), 'only event parameter'),
            ('integer conversion', '#include "api.h"\ndouble f(ext_event e) { return e.instrument_id; }', (), 'implicit conversions'),
            ('generated collision', 'double f() { return 1.0; } double f_context() { return 2.0; }', (), 'conflicts'),
            ('contract alias collision', 'double contract_t() { return 1.0; }', (), 'conflicts'),
            ('standard namespace collision', 'double std() { return 1.0; }', (), 'conflicts'),
        ]
        for label, source, options, reason in cases:
            with self.subTest(label=label):
                self.output.write_text('preserved')
                result = self.compile(source, *options)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(reason, result.stderr)
                self.assertRegex(result.stderr, r':\d+:\d+: error:')
                self.assertEqual(result.stdout, '')
                self.assertEqual(self.output.read_text(), 'preserved')
        self.api.write_text('double read(double);')
        for expression in ['read(read(x))', 'read(x * 2.0)']:
            result = self.compile('#include "api.h"\ndouble f(double x) { return ' + expression + '; }', '--context-function', 'read')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('context arguments', result.stderr)

    def test_record_rejections(self):
        for declaration in [
            'struct event { int* p; };',
            'struct event { double x[2]; };',
            'struct event { int x : 2; };',
            'struct event { int x = 0; };',
            'struct event { mutable int x; };',
            'struct event { volatile int x; };',
            'struct event { private: int x; };',
            'struct event { double method(); };',
            'struct base {}; struct event : base { int x; };',
            'struct nested { int x; }; struct event { nested x; };',
            'struct event { event(); int x; };',
            'union event { int x; double y; };',
        ]:
            with self.subTest(declaration=declaration):
                self.api.write_text(declaration)
                result = self.compile('#include "api.h"\ndouble f() { return 1.0; }')
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(result.stderr, r':\d+:\d+: error:')

    def test_library_validation_and_cli(self):
        library = self.root / 'library.dsl.h'
        for body in ['#define X 1.0\ndouble helper() { return X; }',
                     'double helper() { double x = 1.0; x += 2.0; return x; }',
                     'double helper() { return __DBL_MAX__; }']:
            library.write_text(body)
            result = self.compile('#include "library.dsl.h"\ndouble f() { return 0.0; }', '--dsl-library', library)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('library.dsl.h', result.stderr)
        library.write_text('double helper() { return 1.0; }')
        self.assertNotEqual(self.compile('#include "library.dsl.h"\ndouble f() { return helper(); }').returncode, 0)
        original = library.read_text()
        self.source.write_text('double f() { return 1.0; }')
        result = run(args.dslc, self.source, '--emit-objects', '--dsl-library', library, '-o', library)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(library.read_text(), original)
        result = run(args.dslc, self.source, '--context-function', 'read', '-o', self.output)
        self.assertNotEqual(result.returncode, 0)
        result = self.compile('double f() { return 1.0; }', '--dsl-library', self.api)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('both', result.stderr)


    def test_control_flow_and_record_results(self):
        self.ok(self.compile('''#include "api.h"
struct Answer { double value; int code; bool accepted; };
bool identity(bool value) { const bool local = value; return local; }
int identifier(int value) { const int local = value; return local; }
Answer choose(ext_event event) {
    ext_event copy = event;
    copy.reference_price = 3.0;
    double value = event.reference_price;
    if (!identity(event.enabled)) return Answer{.code = 1};
    if (identifier(event.instrument_id) == 7) {
        double value = 100.0;
        copy.reference_price = value;
    } else {
        value = value + 4.0;
    }
    if (value > 10.0) value = 10.0;
    Answer result = event.enabled ? Answer{value, 0, true} : Answer{};
    result.value = result.value + copy.reference_price;
    return result;
}
'''))
        self.execute('''#include "generated.h"
int main() {
    auto a = choose{}({}, {}, {7, 15.0, true});
    auto b = choose{}({}, {}, {8, 2.0, true});
    auto c = choose{}({}, {}, {7, 15.0, false});
    return a && b && c && a->result.value == 110.0 && b->result.value == 9.0 &&
           c->result.code == 1 && c->result.value == 0.0 && !c->result.accepted ? 0 : 1;
}
''')

    def test_stateful_composition_and_error_rollback(self):
        self.ok(self.compile('''
struct Event { double price; bool valid; };
struct State { double total; };
struct Result { double total; bool positive; };
struct Error { int code; };
Result accumulate(Event event, State& state) {
    if (!event.valid || event.price < 0.0) throw Error{.code = 1};
    state.total = state.total + event.price;
    if (state.total > 100.0) throw Error{.code = 2};
    return Result{state.total, state.total > 0.0};
}
Result compose(Event event, State& state) {
    Result first = accumulate(event, state);
    if (first.positive && event.valid) return accumulate(event, state);
    return first;
}
'''))
        self.assertNotIn('throw ', self.output.read_text())
        self.assertIn('std::unexpected', self.output.read_text())
        self.execute('''#include "generated.h"
#include <type_traits>
int main() {
    static_assert(std::is_same_v<compose_error, Error>);
    static_assert(std::is_same_v<compose_state, State>);
    dsl_runtime::unit<compose> unit(State{0.0}), other(State{40.0});
    auto a = unit.on_event({}, {10.0, true});
    if (!a || a->result.total != 20.0 || unit.state().total != 20.0) return 1;
    auto b = unit.on_event({}, {10.0, false});
    if (b || b.error().code != 1 || unit.state().total != 20.0) return 2;
    // The first child succeeds with 70; the second fails with 120.
    auto c = unit.on_event({}, {50.0, true});
    if (c || c.error().code != 2 || unit.state().total != 20.0) return 3;
    const State input{0.0};
    auto direct = compose{}({}, input, {10.0, true});
    return direct && direct->new_state.total == 20.0 && input.total == 0.0 &&
           other.state().total == 40.0 ? 0 : 4;
}
''', '-fno-exceptions')

    def test_lazy_fallible_helpers(self):
        self.ok(self.compile('''
struct Error { int code; };
bool fail() { throw Error{7}; }
bool both(bool enabled) { return enabled && fail(); }
bool either(bool enabled) { return enabled || fail(); }
double select(bool enabled) { return enabled ? 3.0 : (fail() ? 1.0 : 2.0); }
bool branch(bool enabled) { if (enabled) return true; else return fail(); }
'''))
        self.execute('''#include "generated.h"
int main() {
    auto a = both{}({}, {}, {false});
    auto b = either{}({}, {}, {true});
    auto c = select{}({}, {}, {true});
    auto d = branch{}({}, {}, {true});
    if (!a || a->result || !b || !b->result || !c || c->result != 3.0 || !d || !d->result) return 1;
    auto e = both{}({}, {}, {true});
    auto f = either{}({}, {}, {false});
    auto g = select{}({}, {}, {false});
    auto h = branch{}({}, {}, {false});
    return !e && !f && !g && !h && e.error().code == 7 && h.error().code == 7 ? 0 : 2;
}
''', '-fno-exceptions')

    def test_state_and_value_parameter_are_independent(self):
        self.ok(self.compile('''
struct State { double value; };
double child(State event, State& state) {
    state.value = state.value + 1.0;
    return event.value;
}
double parent(State& state) {
    double previous = child(state, state);
    return previous + state.value;
}
'''))
        self.execute('''#include "generated.h"
int main() {
    dsl_runtime::unit<parent> unit(State{2.0});
    auto value = unit.on_event({}, {});
    return value && value->result == 5.0 && unit.state().value == 3.0 ? 0 : 1;
}
''')

    def test_feature_rejections(self):
        cases = [
            ('missing return', 'double f(bool b) { if (b) return 1.0; }', 'all control paths'),
            ('unreachable', 'double f() { return 1.0; double x = 2.0; }', 'unreachable'),
            ('numeric condition', 'double f(double x) { if (x) return 1.0; return 0.0; }', 'implicit conversions'),
            ('compound assignment', 'double f() { double x = 1.0; x += 2.0; return x; }', 'unsupported'),
            ('integer arithmetic', 'int f(int x) { return x + 1; }', 'double'),
            ('scalar error', 'double f() { throw 1; }', 'flat record'),
            ('mixed errors', 'struct E { int code; }; struct F { int code; }; double f(bool b) { if (b) throw E{1}; throw F{2}; }', 'one flat record'),
            ('composed errors', 'struct E { int code; }; struct F { int code; }; double a() { throw E{1}; } double b(bool x) { if(x) throw F{2}; return a(); }', 'share one error'),
            ('local state argument', 'struct S { double x; }; double a(S& s) { return s.x; } double b() { S s{}; return a(s); }', 'explicit state'),
            ('reference event', 'struct E { double x; }; double a(const E& e) { return e.x; }', 'parameters'),
            ('mutable context alias', '#include "api.h"\ndouble f(ext_event e) { int id = e.instrument_id; id = 2; return get_bid(id); }', 'context arguments'),
            ('state context', '#include "api.h"\nstruct S { int id; }; double f(ext_event e, S& s) { return get_bid(s.id); }', 'context arguments'),
            ('read after guard', '#include "api.h"\ndouble f(ext_event e) { if (!e.enabled) return 0.0; return get_bid(e.instrument_id); }', 'possible return/error'),
            ('read after failure', '#include "api.h"\nstruct E { int code; }; double fail(bool x) { if (x) throw E{1}; return 0.0; } double f(ext_event e) { double x = fail(e.enabled); return get_bid(e.instrument_id); }', 'possible return/error'),
        ]
        for label, source, diagnostic in cases:
            with self.subTest(label=label):
                self.output.write_text('preserved')
                options = ('--context-function', 'get_bid') if '#include' in source else ()
                result = self.compile(source, *options)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(diagnostic, result.stderr)
                self.assertEqual(self.output.read_text(), 'preserved')

    def test_config_imports_and_library_execution(self):
        import json
        package = self.root / 'package'
        package.mkdir()
        library = package / 'pricing.dsl.h'
        library.write_text('#pragma once\n#include "api.h"\ndouble price(ext_event e) { double x = get_bid(e.instrument_id); x = x + 2.0; return x; }')
        dependency = package / 'library.json'
        dependency.write_text(json.dumps({'emit_objects': True, 'external_headers': ['../api.h'],
                                         'dsl_libraries': ['pricing.dsl.h'], 'context_functions': ['get_bid']}))
        config = self.root / 'project.json'
        config.write_text(json.dumps({'imports': ['package/library.json', 'package/library.json'], 'emit_objects': False}))
        self.source.write_text('#include "pricing.dsl.h"\ndouble composed(ext_event e) { return price(e); }')
        # No --emit-objects, library or external-header flags needed; CLI providers compose.
        self.ok(run(args.dslc, self.source, '--config', config, '--context-function', 'get_ask', '-o', self.output))
        self.execute('''#include "generated.h"
double get_bid(int id) { return id == 7 ? 20.0 : 0.0; }
int main() {
    ext_event e{7, 0.0, true};
    auto result = composed{}(prepare_composed_context(e), {}, e);
    return result && result->result == 22.0 ? 0 : 1;
}
''')
        original = dependency.read_text()
        result = run(args.dslc, self.source, '--config', config, '-o', dependency)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(dependency.read_text(), original)

    def test_stateful_library_example(self):
        example = args.examples / 'stateful'
        self.output = self.root / 'accumulate.generated.h'
        self.ok(run(args.dslc, example / 'accumulate.dsl.cpp', '--config',
                    example / 'project.json', '-o', self.output))
        self.execute((example / 'driver.cpp').read_text())

    def test_config_diagnostics(self):
        config = self.root / 'project.json'
        self.source.write_text('double f() { return 1.0; }')
        for content, diagnostic in [
            ('{', 'project.json'), ('[]', 'JSON object'),
            ('{"unknown": []}', 'unknown config key'),
            ('{"emit_objects": "yes"}', 'boolean'),
            ('{"dsl_libraries": "file"}', 'array of strings'),
            ('{"context_functions": [3]}', 'nonempty strings'),
            ('{"imports": ["project.json"]}', 'cyclic'),
            ('{"external_headers": ["missing.h"]}', 'existing regular file'),
            ('{"imports": ["missing.json"]}', 'existing regular file'),
        ]:
            with self.subTest(content=content):
                config.write_text(content)
                self.output.write_text('preserved')
                result = run(args.dslc, self.source, '--config', config, '-o', self.output)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)
                self.assertEqual(self.output.read_text(), 'preserved')
        result = run(args.dslc, self.source, '--config')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--config', result.stderr)


if __name__ == '__main__':
    unittest.main(argv=['objects.py'], verbosity=2)
