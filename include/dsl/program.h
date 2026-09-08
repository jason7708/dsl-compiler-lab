#pragma once
#include "dsl/ir.h"
namespace dsl {
struct CppRecord {
    ir::TypeId type;
    std::string name;
    std::vector<std::string> fields;
    bool external;
    ir::ConstantId initial;
};
struct CppLinkage {
    std::vector<std::string> headers, macros, externalSymbols;
    std::vector<CppRecord> records;
};
// Host acquisition expressions have no access to computation state or temporaries.
struct BindingArgument {
    std::variant<std::size_t, ir::ConstantId> source; // core parameter index or constant
    std::vector<ir::FieldId> fields;
};
struct ContextBinding {
    std::size_t parameter;
    std::string field;
    ir::ExternalId provider;
    std::vector<BindingArgument> arguments;
};
struct HostBindings {
    std::vector<std::vector<ContextBinding>> functions;
};
struct Export {
    ir::FunctionId function;
    std::string name;
    std::vector<std::size_t> eventParameters;
    std::vector<std::string> eventFields;
    std::optional<std::size_t> stateParameter;
    std::optional<ir::ConstantId> initialState;
};
// Entrypoints are ordinary core functions sharing one unit-owned state.
struct UnitGroup {
    std::string name;
    std::vector<ir::FunctionId> entries;
};
struct UnitEnvelope {
    enum class Style { Scalar, Objects };
    Style style;
    std::vector<Export> exports;
    std::vector<UnitGroup> groups = {};
};
struct Program {
    ir::Module computation;
    CppLinkage cpp;
    HostBindings bindings;
    UnitEnvelope units;
};
} // namespace dsl
