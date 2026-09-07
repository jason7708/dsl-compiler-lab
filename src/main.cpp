#include "dsl/codegen.h"
#include "dsl/config.h"
#include "dsl/frontend.h"
#include "dsl/ir.h"
#include "dsl/verify.h"

#include <clang/Basic/Version.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {
constexpr std::string_view usage =
    "Usage: dslc [--dump-ir] [--config <path>]... [--extern-header <path>]... [--emit-objects] "
    "<input.cpp> -o "
    "<output.cpp>\n"
    "       [--context-function <qualified-name>]... [--dsl-library <path>]...\n"
    "       dslc --version\n"
    "       dslc --help\n";
struct Options {
    std::string_view input;
    std::string_view output;
    bool dumpIR = false;
    std::vector<std::string> externalHeaders;
    std::vector<std::string> dslLibraries;
    std::vector<std::string> contextFunctions;
    bool objects = false;
    std::vector<std::string> configFiles;
};

[[nodiscard]] std::expected<Options, std::string> parseOptions(std::span<char *const> arguments) {
    Options options;
    bool positionalOnly = false;
    while (!arguments.empty()) {
        const std::string_view arg = arguments.front();
        arguments = arguments.subspan(1);
        if (!positionalOnly && arg == "--") {
            positionalOnly = true;
        } else if (!positionalOnly && arg == "--dump-ir") {
            options.dumpIR = true;
        } else if (!positionalOnly && arg == "--config") {
            if (arguments.empty())
                return std::unexpected("--config requires a JSON file path");
            auto config = dsl::readConfig(arguments.front());
            if (!config)
                return std::unexpected(config.error());
            options.externalHeaders.append_range(config->externalHeaders);
            options.dslLibraries.append_range(config->dslLibraries);
            options.contextFunctions.append_range(config->contextFunctions);
            options.configFiles.append_range(config->files);
            options.objects = options.objects || config->objects;
            arguments = arguments.subspan(1);
        } else if (!positionalOnly && arg == "--emit-objects") {
            options.objects = true;
        } else if (!positionalOnly && arg == "--context-function") {
            if (arguments.empty())
                return std::unexpected("--context-function requires a qualified function name");
            options.contextFunctions.emplace_back(arguments.front());
            arguments = arguments.subspan(1);
        } else if (!positionalOnly && (arg == "--extern-header" || arg == "--dsl-library")) {
            if (arguments.empty())
                return std::unexpected(std::format("{} requires a header path", arg));
            std::error_code error;
            const auto path = std::filesystem::canonical(arguments.front(), error);
            if (error || !std::filesystem::is_regular_file(path, error))
                return std::unexpected("external header must be an existing regular file");
            const auto name = path.string();
            if (name.find_first_of("\"\\\n\r") != std::string::npos)
                return std::unexpected("external header path contains unsupported characters");
            if (arg == "--extern-header")
                options.externalHeaders.push_back(name);
            else
                options.dslLibraries.push_back(name);
            arguments = arguments.subspan(1);
        } else if (!positionalOnly && arg == "-o") {
            if (!options.output.empty() || arguments.empty())
                return std::unexpected("-o requires exactly one output path");
            options.output = arguments.front();
            arguments = arguments.subspan(1);
        } else if (!positionalOnly && arg.starts_with('-')) {
            return std::unexpected(std::format("unknown option: {}", arg));
        } else if (!options.input.empty()) {
            return std::unexpected("exactly one input file is required");
        } else {
            options.input = arg;
        }
    }
    if (options.input.empty() || options.output.empty())
        return std::unexpected("input and -o output are required");
    if (!options.objects && (!options.contextFunctions.empty() || !options.dslLibraries.empty()))
        return std::unexpected("context functions and DSL libraries require --emit-objects");
    for (const auto &external : options.externalHeaders)
        for (const auto &library : options.dslLibraries) {
            std::error_code error;
            if (std::filesystem::equivalent(external, library, error))
                return std::unexpected(
                    "a header cannot be both an external interface and a DSL library");
        }
    return options;
}

// The LLVM TempFile API requires explicit keep/discard. Keep its cleanup in one
// scope so every failure path, including exception unwinding, discards the file.
class OutputFile final {
  public:
    explicit OutputFile(llvm::sys::fs::TempFile file) : file_(std::move(file)) {}
    OutputFile(const OutputFile &) = delete;
    OutputFile &operator=(const OutputFile &) = delete;
    ~OutputFile() {
        if (!committed_)
            llvm::consumeError(file_.discard());
    }

    [[nodiscard]] std::expected<void, std::string> write(std::string_view text,
                                                         std::string_view path) {
        llvm::raw_fd_ostream stream(file_.FD, false);
        stream << llvm::StringRef(text);
        stream.flush();
        if (stream.has_error()) {
            const auto reason = stream.error().message();
            stream.clear_error();
            return std::unexpected(std::format("cannot write output: {}", reason));
        }
        if (auto error = file_.keep(llvm::StringRef(path)))
            return std::unexpected(
                std::format("cannot save output '{}': {}", path, llvm::toString(std::move(error))));
        committed_ = true;
        return {};
    }

  private:
    llvm::sys::fs::TempFile file_;
    bool committed_ = false;
};

[[nodiscard]] std::expected<void, std::string> writeOutput(std::string_view path,
                                                           std::string_view text) {
    auto temporary = llvm::sys::fs::TempFile::create(std::format("{}.tmp-%%%%%%", path));
    if (!temporary)
        return std::unexpected(std::format("cannot create output '{}': {}", path,
                                           llvm::toString(temporary.takeError())));
    return OutputFile{std::move(*temporary)}.write(text, path);
}

int fail(std::string_view reason) {
    std::println(stderr, "dslc: error: {}", reason);
    return 1;
}
} // namespace

int main(int argc, char **argv) {
    const auto arguments = std::span{argv, static_cast<std::size_t>(argc)}.subspan(1);
    if (arguments.size() == 1 && std::string_view(arguments.front()) == "--help") {
        std::print("{}", usage);
        return 0;
    }
    if (arguments.size() == 1 && std::string_view(arguments.front()) == "--version") {
        std::println("dslc 0.1.0\nLLVM {}\nClang {}", DSL_LLVM_VERSION,
                     clang::getClangFullVersion());
        return 0;
    }
    const auto options = parseOptions(arguments);
    if (!options) {
        std::print(stderr, "{}", usage);
        return fail(options.error());
    }
    const auto &[input, output, dumpIR, externalHeaders, dslLibraries, contextFunctions, objects,
                 configFiles] = *options;
    std::error_code error;
    if (std::filesystem::equivalent(input, output, error))
        return fail("input and output must be different files");
    for (const auto &header : externalHeaders) {
        if (std::filesystem::equivalent(header, output, error))
            return fail("external header and output must be different files");
    }
    for (const auto &library : dslLibraries) {
        if (std::filesystem::equivalent(library, output, error))
            return fail("DSL library and output must be different files");
    }
    for (const auto &file : configFiles) {
        if (std::filesystem::equivalent(file, output, error))
            return fail("config and output must be different files");
    }
    const auto source = llvm::MemoryBuffer::getFile(llvm::StringRef(input));
    if (!source)
        return fail(std::format("cannot read '{}': {}", input, source.getError().message()));
    auto module = dsl::compile((*source)->getBuffer(), input,
                               {.externalHeaders = externalHeaders,
                                .dslLibraries = dslLibraries,
                                .contextFunctions = contextFunctions,
                                .objects = objects});
    if (!module)
        return fail(module.error());
    if (auto verified = dsl::ir::verify(module->computation); !verified)
        return fail(verified.error());
    auto generated = dsl::cpp::generate(*module);
    if (!generated)
        return fail(generated.error());
    if (const auto saved = writeOutput(output, *generated); !saved)
        return fail(saved.error());
    if (dumpIR)
        std::print("{}", dsl::ir::format(module->computation));
    return 0;
}
