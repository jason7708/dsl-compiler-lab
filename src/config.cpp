#include "dsl/config.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

#include <filesystem>
#include <format>
#include <unordered_set>

namespace dsl {
namespace {
std::expected<std::string, std::string> filePath(const std::filesystem::path &path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_regular_file(canonical, error))
        return std::unexpected("config path must identify an existing regular file: " +
                               path.string());
    const auto name = canonical.string();
    if (name.find_first_of("\"\\\n\r") != std::string::npos)
        return std::unexpected("unsupported characters in config path: " + name);
    return name;
}
class Reader {
  public:
    std::expected<void, std::string> read(const std::filesystem::path &path, unsigned depth = 0) {
        const auto file = filePath(path);
        if (!file)
            return std::unexpected(file.error());
        if (active_.contains(*file))
            return std::unexpected("cyclic config imports: " + *file);
        if (seen_.contains(*file))
            return {};
        if (depth > 64)
            return std::unexpected("config import depth exceeds 64: " + *file);
        active_.insert(*file);
        const auto content = llvm::MemoryBuffer::getFile(*file);
        if (!content)
            return std::unexpected("cannot read config: " + *file);
        auto value = llvm::json::parse((*content)->getBuffer());
        if (!value)
            return std::unexpected(*file + ": " + llvm::toString(value.takeError()));
        const auto *object = value->getAsObject();
        if (!object)
            return std::unexpected(*file + ": config must be a JSON object");
        for (const auto &[key, field] : *object) {
            const auto name = key.str();
            if (name != "imports" && name != "emit_objects" && name != "external_headers" &&
                name != "dsl_libraries" && name != "context_functions")
                return std::unexpected(*file + ": unknown config key: " + name);
        }
        const auto directory = std::filesystem::path(*file).parent_path();
        auto strings = [&](llvm::StringRef key,
                           auto &&consume) -> std::expected<void, std::string> {
            const auto *field = object->get(key);
            if (!field)
                return {};
            const auto *array = field->getAsArray();
            if (!array)
                return std::unexpected(*file + ": " + key.str() + " must be an array of strings");
            for (const auto &item : *array) {
                const auto text = item.getAsString();
                if (!text || text->empty() || text->contains('\0'))
                    return std::unexpected(*file + ": " + key.str() +
                                           " entries must be nonempty strings without NUL");
                if (auto result = consume(text->str()); !result)
                    return result;
            }
            return {};
        };
        auto imports = strings(
            "imports", [&](const std::string &name) { return read(directory / name, depth + 1); });
        if (!imports)
            return imports;
        if (const auto *field = object->get("emit_objects")) {
            const auto enabled = field->getAsBoolean();
            if (!enabled)
                return std::unexpected(*file + ": emit_objects must be a boolean");
            result.objects = result.objects || *enabled;
        }
        auto paths = [&](llvm::StringRef key, std::vector<std::string> &destination) {
            return strings(key, [&](const std::string &name) -> std::expected<void, std::string> {
                const auto path = filePath(directory / name);
                if (!path)
                    return std::unexpected(*file + ": " + path.error());
                destination.push_back(*path);
                return {};
            });
        };
        if (auto fields = paths("external_headers", result.externalHeaders); !fields)
            return fields;
        if (auto fields = paths("dsl_libraries", result.dslLibraries); !fields)
            return fields;
        auto functions = strings("context_functions",
                                 [&](const std::string &name) -> std::expected<void, std::string> {
                                     result.contextFunctions.push_back(name);
                                     return {};
                                 });
        if (!functions)
            return functions;
        result.files.push_back(*file);
        active_.erase(*file);
        seen_.insert(*file);
        return {};
    }
    ProjectConfig result;

  private:
    std::unordered_set<std::string> active_, seen_;
};
} // namespace
std::expected<ProjectConfig, std::string> readConfig(std::string_view path) {
    Reader reader;
    if (const auto result = reader.read(std::filesystem::path(path)); !result)
        return std::unexpected(result.error());
    return std::move(reader.result);
}
} // namespace dsl
