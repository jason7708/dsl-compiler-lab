#pragma once

#include "dsl/ir.h"
#include <string>

namespace dsl {
[[nodiscard]] std::string generateCpp(const Module &module);
} // namespace dsl
