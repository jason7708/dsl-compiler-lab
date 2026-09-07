#pragma once
#include "dsl/program.h"
#include <expected>
namespace dsl::cpp {
// Verifies core IR and C++/host integration compatibility before emission.
[[nodiscard]] std::expected<std::string, std::string> generate(const Program &);
} // namespace dsl::cpp
