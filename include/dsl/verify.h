#pragma once
#include "dsl/registry.h"
#include <expected>
namespace dsl::ir {
[[nodiscard]] std::expected<void, std::string>
verify(const Module &, std::span<const OpDefinition> definitions = registry());
} // namespace dsl::ir
