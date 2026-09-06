#pragma once
#include "dsl/ir.h"
#include <expected>

namespace dsl {
// Inline acyclic DSL calls, identify context reads and validate their dependencies.
[[nodiscard]] std::expected<Module, std::string> lowerObjects(Module module);
[[nodiscard]] std::string generateObjects(const Module &module);
} // namespace dsl
