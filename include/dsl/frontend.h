#pragma once
#include "dsl/program.h"
#include <expected>
#include <span>
#include <string_view>
namespace dsl {
struct CompileOptions {
    std::span<const std::string> externalHeaders;
    std::span<const std::string> dslLibraries;
    std::span<const std::string> contextFunctions;
    bool objects = false;
};
// Clang diagnostics retain source positions; semantic/IR failures return a reason.
[[nodiscard]] std::expected<Program, std::string>
compile(std::string_view source, std::string_view filename, const CompileOptions &options = {});
} // namespace dsl
