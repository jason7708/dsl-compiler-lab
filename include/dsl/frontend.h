#pragma once

#include "dsl/ir.h"
#include <optional>
#include <span>
#include <string_view>

namespace dsl {
struct CompileOptions {
    std::span<const std::string> externalHeaders;
    std::span<const std::string> dslLibraries;
    std::span<const std::string> contextFunctions;
    bool objects = false;
};
// Diagnostics are emitted by Clang with input filename, line and column.
[[nodiscard]] std::optional<Module> compile(std::string_view source, std::string_view filename,
                                            const CompileOptions &options = {});
} // namespace dsl
