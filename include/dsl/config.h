#pragma once
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace dsl {
struct ProjectConfig {
    std::vector<std::string> externalHeaders;
    std::vector<std::string> dslLibraries;
    std::vector<std::string> contextFunctions;
    std::vector<std::string> files;
    bool objects = false;
};
[[nodiscard]] std::expected<ProjectConfig, std::string> readConfig(std::string_view path);
} // namespace dsl
