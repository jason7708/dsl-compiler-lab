#include "dsl/frontend.h"
#include "dsl/math.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/Tooling.h>

#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace dsl {
namespace {
constexpr std::string_view runtimeHeaderName = "dsl_runtime/math.h";
const std::string runtimeHeaderPath = std::string(DSL_RUNTIME_INCLUDE_DIR) + "/dsl_runtime/math.h";

bool isRuntimeLocation(clang::CompilerInstance &compiler, clang::SourceLocation location) {
    const auto &sources = compiler.getSourceManager();
    const auto file = sources.getFileEntryRefForID(sources.getFileID(location));
    const auto runtime = compiler.getFileManager().getOptionalFileRef(runtimeHeaderPath);
    return file && runtime && file->getUniqueID() == runtime->getUniqueID();
}

struct HeaderPolicy {
    const CompileOptions &options;
    std::vector<std::string> included;
    std::vector<std::string> macros;

    std::optional<std::size_t> find(clang::CompilerInstance &compiler,
                                    clang::OptionalFileEntryRef file) const {
        if (!file)
            return std::nullopt;
        for (const auto [id, path] : options.externalHeaders | std::views::enumerate) {
            const auto expected = compiler.getFileManager().getOptionalFileRef(path);
            if (expected && file->getUniqueID() == expected->getUniqueID())
                return id;
        }
        return std::nullopt;
    }
    bool library(clang::CompilerInstance &compiler, clang::OptionalFileEntryRef file) const {
        if (!file)
            return false;
        for (const auto &path : options.dslLibraries) {
            const auto expected = compiler.getFileManager().getOptionalFileRef(path);
            if (expected && expected->getUniqueID() == file->getUniqueID())
                return true;
        }
        return false;
    }
    bool dsl(clang::CompilerInstance &compiler, clang::SourceLocation location) const {
        const auto &sources = compiler.getSourceManager();
        location = sources.getSpellingLoc(location);
        return sources.isWrittenInMainFile(location) ||
               library(compiler, sources.getFileEntryRefForID(sources.getFileID(location)));
    }

    bool contains(clang::CompilerInstance &compiler, clang::SourceLocation location) const {
        const auto &sources = compiler.getSourceManager();
        return find(compiler, sources.getFileEntryRefForID(
                                  sources.getFileID(sources.getSpellingLoc(location))))
            .has_value();
    }
};

void reject(clang::DiagnosticsEngine &diagnostics, clang::SourceLocation location,
            const std::string &reason) {
    const unsigned id = diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error, "DSL: %0");
    diagnostics.Report(location, id) << reason;
}

// Recognize literal includes; resolved file identity is checked by PPCallbacks. Tokenization,
// include resolution, and all C++ parsing remain Clang's responsibility.
bool checkInclude(clang::Lexer &lexer, clang::CompilerInstance &compiler,
                  clang::SourceLocation location, const HeaderPolicy &headers) {
    auto &sources = compiler.getSourceManager();
    clang::Token token;
    auto next = [&] {
        lexer.LexFromRawLexer(token);
        return !token.is(clang::tok::eof) && !token.isAtStartOfLine();
    };
    auto spelling = [&] {
        return clang::Lexer::getSpelling(token, sources, compiler.getLangOpts());
    };
    const bool hasDirective = next();
    if (hasDirective && spelling() == "pragma" && !sources.isWrittenInMainFile(location) &&
        next() && spelling() == "once")
        return true;
    if (!hasDirective || spelling() != "include") {
        reject(compiler.getDiagnostics(), location,
               "preprocessor directives other than literal includes are not supported");
        return false;
    }
    std::string header;
    if (next()) {
        if (token.is(clang::tok::string_literal)) {
            const auto quoted = spelling();
            header = quoted.substr(1, quoted.size() - 2);
        } else if (token.is(clang::tok::less)) {
            while (next() && !token.is(clang::tok::greater))
                header += spelling();
            if (!token.is(clang::tok::greater))
                header.clear();
        }
    }
    if (header.empty() ||
        (header != runtimeHeaderName &&
         (headers.options.externalHeaders.empty() && headers.options.dslLibraries.empty()))) {
        reject(compiler.getDiagnostics(), location,
               "only #include <dsl_runtime/math.h> is supported");
        return false;
    }
    return true;
}

// ASTs omit directives and some attributes/extensions. Use Clang's raw lexer
// only as a token allowlist, before preprocessing; Clang still does all parsing.
bool checkTokens(clang::CompilerInstance &compiler, const HeaderPolicy &headers,
                 clang::FileID input = {}) {
    auto &sources = compiler.getSourceManager();
    auto &pp = compiler.getPreprocessor();
    const auto file = input.isInvalid() ? sources.getMainFileID() : input;
    clang::Lexer lexer(file, sources.getBufferOrFake(file), sources, compiler.getLangOpts());
    clang::Token token;
    while (true) {
        lexer.LexFromRawLexer(token);
        if (token.is(clang::tok::eof))
            return true;
        if (token.is(clang::tok::hash)) {
            if (!checkInclude(lexer, compiler, token.getLocation(), headers))
                return false;
            continue;
        }
        if (token.is(clang::tok::raw_identifier)) {
            pp.LookUpIdentifierInfo(token);
            const auto spelling = token.getIdentifierInfo()->getName();
            if (spelling == "_Pragma" || spelling == "__pragma") {
                reject(compiler.getDiagnostics(), token.getLocation(), "pragmas are not supported");
                return false;
            }
        }
        if (headers.options.objects &&
            (token.is(clang::tok::period) || token.is(clang::tok::kw_int) ||
             token.is(clang::tok::kw_struct) || token.is(clang::tok::kw_if) ||
             token.is(clang::tok::kw_else) || token.is(clang::tok::kw_throw) ||
             token.is(clang::tok::amp) || token.is(clang::tok::ampamp) ||
             token.is(clang::tok::pipepipe) || token.is(clang::tok::exclaim)))
            continue;
        switch (token.getKind()) {
        case clang::tok::identifier:
        case clang::tok::kw_bool:
        case clang::tok::kw_true:
        case clang::tok::kw_false:
        case clang::tok::kw_double:
        case clang::tok::kw_const:
        case clang::tok::kw_return:
        case clang::tok::numeric_constant:
        case clang::tok::plus:
        case clang::tok::minus:
        case clang::tok::star:
        case clang::tok::slash:
        case clang::tok::less:
        case clang::tok::lessequal:
        case clang::tok::greater:
        case clang::tok::greaterequal:
        case clang::tok::equalequal:
        case clang::tok::exclaimequal:
        case clang::tok::question:
        case clang::tok::coloncolon:
        case clang::tok::colon:
        case clang::tok::equal:
        case clang::tok::l_paren:
        case clang::tok::r_paren:
        case clang::tok::l_brace:
        case clang::tok::r_brace:
        case clang::tok::semi:
        case clang::tok::comma:
            break;
        default:
            reject(compiler.getDiagnostics(), token.getLocation(),
                   token.is(clang::tok::hash)
                       ? "preprocessor directives are not supported"
                       : "unsupported token '" +
                             clang::Lexer::getSpelling(token, sources, compiler.getLangOpts()) +
                             "'");
            return false;
        }
    }
}

class MacroChecker final : public clang::PPCallbacks {
  public:
    MacroChecker(clang::CompilerInstance &compiler, HeaderPolicy &headers)
        : compiler_(compiler), headers_(headers) {}
    void InclusionDirective(clang::SourceLocation location, const clang::Token &,
                            llvm::StringRef name, bool, clang::CharSourceRange,
                            clang::OptionalFileEntryRef file, llvm::StringRef, llvm::StringRef,
                            const clang::Module *, bool,
                            clang::SrcMgr::CharacteristicKind) override {
        if (name != llvm::StringRef(runtimeHeaderName)) {
            if (headers_.library(compiler_, file))
                return;
            if (const auto id = headers_.find(compiler_, file)) {
                const auto &path = headers_.options.externalHeaders[*id];
                if (std::ranges::find(headers_.included, path) == headers_.included.end())
                    headers_.included.push_back(path);
                return;
            }
        }
        const auto runtime = compiler_.getFileManager().getOptionalFileRef(runtimeHeaderPath);
        if (name != llvm::StringRef(runtimeHeaderName) || !file || !runtime ||
            file->getUniqueID() != runtime->getUniqueID()) {
            reject(compiler_.getDiagnostics(), location,
                   "include must resolve to the configured runtime header or an authorized "
                   "external header");
        }
    }
    void FileChanged(clang::SourceLocation location, FileChangeReason reason,
                     clang::SrcMgr::CharacteristicKind, clang::FileID) override {
        const auto &sources = compiler_.getSourceManager();
        const auto file = sources.getFileID(location);
        if (reason == EnterFile && headers_.library(compiler_, sources.getFileEntryRefForID(file)))
            checkTokens(compiler_, headers_, file);
    }
    void MacroDefined(const clang::Token &token, const clang::MacroDirective *) override {
        if (headers_.contains(compiler_, token.getLocation())) {
            const auto name = token.getIdentifierInfo()->getName().str();
            if (std::ranges::find(headers_.macros, name) == headers_.macros.end())
                headers_.macros.push_back(name);
        }
    }
    void MacroExpands(const clang::Token &token, const clang::MacroDefinition &, clang::SourceRange,
                      const clang::MacroArgs *) override {
        if (headers_.dsl(compiler_,
                         compiler_.getSourceManager().getExpansionLoc(token.getLocation()))) {
            reject(compiler_.getDiagnostics(), token.getLocation(),
                   "macro expansions are not supported");
        }
    }

  private:
    clang::CompilerInstance &compiler_;
    HeaderPolicy &headers_;
};

class Lowering final : public clang::ASTConsumer {
  public:
    Lowering(clang::CompilerInstance &compiler, std::optional<Module> &result,
             HeaderPolicy &headers)
        : compiler_(compiler), result_(result), headers_(headers) {}

    void HandleTranslationUnit(clang::ASTContext &context) override {
        if (compiler_.getDiagnostics().hasErrorOccurred())
            return;
        const auto &sources = compiler_.getSourceManager();
        module_.objectMode = headers_.options.objects;
        std::unordered_map<std::string, const clang::FunctionDecl *> names;
        std::vector<const clang::FunctionDecl *> definitions;
        bool hasEntry = false;
        for (const auto *decl : context.getTranslationUnitDecl()->decls()) {
            if (decl->isImplicit())
                continue;
            if (const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(decl);
                space && isRuntimeLocation(compiler_, space->getLocation()) &&
                space->getName() == "dsl_math") {
                for (const auto *member : space->decls()) {
                    const auto *function = llvm::dyn_cast<clang::FunctionDecl>(member);
                    if (!function || !isRuntimeLocation(compiler_, function->getLocation())) {
                        error(member->getLocation(), "unexpected declaration in runtime header");
                        return;
                    }
                    const auto builtin = findMathBuiltin(function->getNameAsString());
                    if (!builtin || !validSignature(function) ||
                        function->getNumParams() != mathBuiltin(*builtin).arity ||
                        function->isThisDeclarationADefinition()) {
                        error(function->getLocation(),
                              "runtime declaration does not match the supported API");
                        return;
                    }
                    builtinDeclarations_.emplace(function->getCanonicalDecl(), *builtin);
                }
                continue;
            }
            if (headers_.contains(compiler_, decl->getLocation())) {
                if (!collectExternal(decl))
                    return;
                continue;
            }
            if (headers_.options.objects && headers_.dsl(compiler_, decl->getLocation())) {
                if (const auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
                    module_.importedNames.push_back(record->getQualifiedNameAsString());
                    if (!collectRecord(record, false))
                        return;
                    continue;
                }
            }
            const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if (!function || !headers_.dsl(compiler_, decl->getLocation()) ||
                !function->getDeclContext()->isTranslationUnit()) {
                error(decl->getLocation(),
                      "only global DSL function declarations and definitions are supported");
                return;
            }
            if (!validSignature(function))
                return;
            if (headers_.contains(compiler_, function->getCanonicalDecl()->getLocation())) {
                error(function->getLocation(),
                      "external functions cannot be redeclared or defined in DSL input");
                return;
            }
            const auto *canonical = function->getCanonicalDecl();
            const auto name = function->getNameAsString();
            const auto [named, inserted] = names.emplace(name, canonical);
            if (!inserted && named->second != canonical) {
                error(function->getLocation(), "function overloading is not supported");
                return;
            }
            if (dslDeclarations_.contains(canonical))
                continue;
            const auto *definition = function->getDefinition();
            if (!definition || !headers_.dsl(compiler_, definition->getLocation())) {
                error(function->getLocation(),
                      "DSL function must have a definition in the input file");
                return;
            }
            const FunctionId id = definitions.size();
            dslDeclarations_.emplace(canonical, id);
            definitions.push_back(definition);
            module_.functions.push_back(Function{.name = name, .values = {}, .body = {}});
            if (name == "compute") {
                module_.entry = id;
                hasEntry = true;
            }
        }
        if (!hasEntry && !headers_.options.objects) {
            error(sources.getLocForStartOfFile(sources.getMainFileID()),
                  "expected exactly one entry function definition named compute");
            return;
        }
        if (definitions.empty()) {
            error(sources.getLocForStartOfFile(sources.getMainFileID()),
                  "expected at least one DSL function");
            return;
        }
        for (const auto &name : headers_.options.contextFunctions) {
            const auto found =
                std::ranges::find(module_.externals, name, &ExternalDeclaration::name);
            if (found == module_.externals.end()) {
                error(sources.getLocForStartOfFile(sources.getMainFileID()),
                      "context function has no imported declaration: " + name);
                return;
            }
            found->context = true;
        }
        // Resolve the whole module before lowering bodies, including forward and
        // recursive calls. Invalid unused helper bodies are checked as well.
        definitions_ = definitions;
        for (const auto [id, definition] : definitions | std::views::enumerate) {
            ir_ = Function{.name = definition->getNameAsString(), .values = {}, .body = {}};
            ir_.location = sourcePosition(definition->getLocation());
            ir_.returnType = *inputType(definition->getReturnType());
            ir_.returnRecord = recordName(definition->getReturnType());
            currentRegion_ = &ir_.body;
            bindings_.clear();
            stateDecl_ = nullptr;
            if (!lowerFunction(definition))
                return;
            module_.functions[id] = std::move(ir_);
        }
        module_.externalHeaders = headers_.included;
        module_.externalMacros = headers_.macros;
        result_ = std::move(module_);
    }

  private:
    bool collectExternal(const clang::Decl *decl) {
        if (decl->isImplicit())
            return true;
        if (!headers_.contains(compiler_, decl->getLocation())) {
            error(decl->getLocation(), "external declarations must come from authorized headers");
            return false;
        }
        if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(decl);
            named && !named->getNameAsString().empty())
            module_.importedNames.push_back(named->getQualifiedNameAsString());
        if (headers_.options.objects) {
            if (const auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(decl))
                return collectRecord(record);
        }
        if (const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
            if (space->isAnonymousNamespace() || space->hasAttrs() ||
                space->getName().starts_with("dsl_function_") ||
                (space->getDeclContext()->getRedeclContext()->isTranslationUnit() &&
                 space->getName() == "dsl_math")) {
                error(space->getLocation(),
                      "external namespaces must be named and must not use reserved DSL names");
                return false;
            }
            for (const auto *member : space->decls())
                if (!collectExternal(member))
                    return false;
            return true;
        }
        if (const auto *linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
            for (const auto *member : linkage->decls())
                if (!collectExternal(member))
                    return false;
            return true;
        }
        const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
        if (!function || llvm::isa<clang::CXXMethodDecl>(function) ||
            function->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate ||
            !function->getIdentifier() || !function->isExternallyVisible() ||
            function->isThisDeclarationADefinition() || !validSignature(function, true)) {
            error(decl->getLocation(), "external headers support only ordinary double function "
                                       "declarations without definitions");
            return false;
        }
        const auto name = function->getQualifiedNameAsString();
        if (name == "compute" || function->getName().starts_with("dsl_function_")) {
            error(function->getLocation(),
                  "external function name conflicts with generated DSL functions");
            return false;
        }
        const auto *canonical = function->getCanonicalDecl();
        if (!headers_.contains(compiler_, canonical->getLocation())) {
            error(function->getLocation(),
                  "external functions cannot redeclare DSL or runtime functions");
            return false;
        }
        const auto [entry, inserted] = externalNames_.emplace(name, canonical);
        if (!inserted && entry->second != canonical) {
            error(function->getLocation(), "external function overloading is not supported");
            return false;
        }
        if (!externalDeclarations_.contains(canonical)) {
            externalDeclarations_.emplace(canonical, module_.externals.size());
            std::vector<Type> parameters;
            for (const auto *parameter : function->parameters())
                parameters.push_back(*inputType(parameter->getOriginalType()));
            module_.externals.push_back({.name = name,
                                         .arity = function->getNumParams(),
                                         .parameters = std::move(parameters)});
        }
        return true;
    }
    bool collectRecord(const clang::CXXRecordDecl *record, bool external = true) {
        if (!record->isThisDeclarationADefinition())
            return true;
        if (!record->isStruct() || !record->getIdentifier() || record->hasAttrs() ||
            !record->isAggregate() || !record->isStandardLayout() ||
            !record->isTriviallyCopyable() || record->getNumBases() != 0 ||
            record->getDescribedClassTemplate()) {
            error(record->getLocation(), "event records must be flat, trivial public structs");
            return false;
        }
        RecordDeclaration result{
            .name = record->getQualifiedNameAsString(), .fields = {}, .external = external};
        for (const auto *member : record->decls()) {
            if (member->isImplicit())
                continue;
            const auto *field = llvm::dyn_cast<clang::FieldDecl>(member);
            const auto type = field ? inputType(field->getType()) : std::nullopt;
            if (!field || !type || *type == Type::Record ||
                field->getAccess() != clang::AS_public || field->isBitField() ||
                field->isMutable() || field->hasInClassInitializer() || field->hasAttrs() ||
                !field->getIdentifier()) {
                error(member->getLocation(),
                      "event fields must be plain double, bool or 32-bit int without initializers");
                return false;
            }
            result.fields.push_back({.name = field->getNameAsString(), .type = *type});
        }
        module_.records.push_back(std::move(result));
        return true;
    }
    std::optional<Type> inputType(clang::QualType type) const {
        if (const auto scalar = scalarType(type))
            return scalar;
        if (!headers_.options.objects || type.hasQualifiers())
            return std::nullopt;
        if (type->isSpecificBuiltinType(clang::BuiltinType::Int) &&
            compiler_.getASTContext().getTypeSize(type) == 32)
            return Type::Int;
        if (const auto *record = type->getAsCXXRecordDecl();
            record && (headers_.contains(compiler_, record->getLocation()) ||
                       headers_.dsl(compiler_, record->getLocation())))
            return Type::Record;
        return std::nullopt;
    }
    std::string recordName(clang::QualType type) const {
        const auto *record = type.getNonReferenceType()->getAsCXXRecordDecl();
        return record ? record->getQualifiedNameAsString() : "";
    }
    bool validSignature(const clang::FunctionDecl *function, bool external = false) {
        const auto result = inputType(function->getReturnType());
        const bool validResult = headers_.options.objects && !external
                                     ? result.has_value()
                                     : plainDouble(function->getReturnType());
        if (!validResult || function->isVariadic() ||
            (function->getStorageClass() != clang::SC_None &&
             !(external && function->getStorageClass() == clang::SC_Extern)) ||
            function->hasAttrs() || function->isInlineSpecified() || function->isConstexpr()) {
            error(function->getLocation(),
                  "DSL functions must have an unqualified double return type and no modifiers");
            return false;
        }
        for (const auto *parameter : function->parameters()) {
            const auto original = parameter->getOriginalType();
            const bool state = headers_.options.objects && !external &&
                               original->isLValueReferenceType() &&
                               inputType(original.getNonReferenceType()) == Type::Record &&
                               parameter == function->getParamDecl(function->getNumParams() - 1);
            const auto type = inputType(original);
            const bool allowed = headers_.options.objects
                                     ? (state || (type && !(external && *type == Type::Record)))
                                     : plainDouble(parameter->getOriginalType());
            if (!allowed || parameter->hasDefaultArg() || parameter->hasAttrs()) {
                error(parameter->getLocation(), "parameters must be unqualified double without "
                                                "default arguments or attributes");
                return false;
            }
        }
        if (headers_.options.objects && !external) {
            unsigned events = 0, records = 0;
            for (const auto *parameter : function->parameters()) {
                if (parameter->getOriginalType()->isReferenceType())
                    continue;
                ++events;
                if (inputType(parameter->getOriginalType()) == Type::Record)
                    ++records;
            }
            if (records && events != 1) {
                error(function->getLocation(), "a record event must be the only event parameter "
                                               "(an optional State& may follow)");
                return false;
            }
        }
        return true;
    }
    bool lowerFunction(const clang::FunctionDecl *function) {
        for (const auto *parameter : function->parameters()) {
            const auto original = parameter->getOriginalType();
            const auto type = *inputType(original.getNonReferenceType());
            const auto record = recordName(original);
            const bool state = original->isReferenceType();
            const auto id = add(Parameter{.name = parameter->getNameAsString(),
                                          .recordName = record,
                                          .state = state},
                                type, record);
            bindings_.emplace(parameter, id);
            if (state) {
                ir_.stateParameter = id;
                ir_.stateRecord = record;
                stateDecl_ = parameter;
            }
        }
        const auto *body = llvm::dyn_cast<clang::CompoundStmt>(function->getBody());
        if (headers_.options.objects) {
            const auto flow = body ? statements(body->body()) : std::nullopt;
            if (!flow)
                return false;
            if (!*flow) {
                error(function->getLocation(),
                      "every path must return a result or throw a typed error");
                return false;
            }
            return true;
        }
        if (!body || body->body_empty() || !llvm::isa<clang::ReturnStmt>(body->body_back())) {
            error(function->getLocation(), "function body must end with one return statement");
            return false;
        }
        for (const auto *statement : body->body()) {
            if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
                if (statement != body->body_back()) {
                    error(ret->getReturnLoc(), "return is allowed only as the final statement");
                    return false;
                }
                const auto value = expression(ret->getRetValue());
                if (!value)
                    return false;
                ir_.body.result = *value;
            } else if (const auto *declarations = llvm::dyn_cast<clang::DeclStmt>(statement)) {
                for (const auto *decl : declarations->decls()) {
                    const auto *local = llvm::dyn_cast<clang::VarDecl>(decl);
                    if (!local || !inputType(local->getType().getUnqualifiedType()) ||
                        inputType(local->getType().getUnqualifiedType()) == Type::Record ||
                        (local->getType().getLocalQualifiers() !=
                             clang::Qualifiers::fromCVRMask(clang::Qualifiers::Const) &&
                         !(headers_.options.objects && !local->getType().hasQualifiers())) ||
                        local->getStorageClass() != clang::SC_None || local->hasAttrs() ||
                        local->isConstexpr() || !local->hasInit()) {
                        error(decl->getLocation(),
                              "locals must be initialized const double or const bool "
                              "variables without other modifiers");
                        return false;
                    }
                    const auto initializer = expression(local->getInit());
                    if (!initializer)
                        return false;
                    // Bind only after initialization: self-reference is forbidden.
                    bindings_.emplace(local, add(Reference{.target = *initializer,
                                                           .name = local->getNameAsString()},
                                                 ir_.values[*initializer].type));
                }
            } else {
                error(statement->getBeginLoc(),
                      std::format("unsupported statement: {}", statement->getStmtClassName()));
                return false;
            }
        }
        return true;
    }

  private:
    static std::optional<Type> scalarType(clang::QualType type) {
        if (type.hasQualifiers())
            return std::nullopt;
        if (type->isSpecificBuiltinType(clang::BuiltinType::Double))
            return Type::Double;
        if (type->isSpecificBuiltinType(clang::BuiltinType::Bool))
            return Type::Bool;
        return std::nullopt;
    }
    static bool plainDouble(clang::QualType type) {
        return type->isSpecificBuiltinType(clang::BuiltinType::Double) && !type.hasQualifiers();
    }
    std::string sourcePosition(clang::SourceLocation location) const {
        const auto position = compiler_.getSourceManager().getPresumedLoc(location);
        return position.isValid() ? std::format("{}:{}:{}", position.getFilename(),
                                                position.getLine(), position.getColumn())
                                  : "input";
    }
    void error(clang::SourceLocation location, const std::string &reason) {
        reject(compiler_.getDiagnostics(), location, reason);
    }
    [[nodiscard]] ValueId add(Operation operation, Type type = Type::Double,
                              std::string record = {}) {
        const ValueId id = ir_.values.size();
        ir_.values.push_back(Value{
            .type = type, .operation = std::move(operation), .recordName = std::move(record)});
        currentRegion_->instructions.push_back(id);
        return id;
    }
    void append(Operation operation) { (void)add(std::move(operation)); }
    std::optional<bool> statements(llvm::iterator_range<clang::Stmt *const *> list) {
        bool exits = false;
        for (const auto *statement : list) {
            if (exits) {
                error(statement->getBeginLoc(),
                      "unreachable statements after return or throw are not supported");
                return std::nullopt;
            }
            auto flow = statementBody(statement);
            if (!flow)
                return std::nullopt;
            exits = *flow;
        }
        return exits;
    }
    std::optional<bool> statementBody(const clang::Stmt *statement) {
        if (const auto *block = llvm::dyn_cast<clang::CompoundStmt>(statement)) {
            auto outer = bindings_;
            Region nested;
            auto *parent = std::exchange(currentRegion_, &nested);
            const auto result = statements(block->body());
            currentRegion_ = parent;
            bindings_ = std::move(outer);
            if (!result)
                return std::nullopt;
            append(Scope{std::move(nested)});
            return result;
        }
        if (const auto *conditional = llvm::dyn_cast<clang::IfStmt>(statement)) {
            if (conditional->getInit() || conditional->getConditionVariable() ||
                conditional->isConstexpr()) {
                error(conditional->getIfLoc(), "if requires a plain bool expression");
                return std::nullopt;
            }
            const auto condition = expression(conditional->getCond());
            if (!condition)
                return std::nullopt;
            if (ir_.values[*condition].type != Type::Bool) {
                error(conditional->getIfLoc(), "if requires a bool condition");
                return std::nullopt;
            }
            const auto outer = bindings_;
            Region yes, no;
            auto *parent = std::exchange(currentRegion_, &yes);
            auto yesFlow = statementBody(conditional->getThen());
            bindings_ = outer;
            currentRegion_ = &no;
            auto noFlow = conditional->getElse() ? statementBody(conditional->getElse())
                                                 : std::optional{false};
            bindings_ = outer;
            currentRegion_ = parent;
            if (!yesFlow || !noFlow)
                return std::nullopt;
            append(If{*condition, std::move(yes), std::move(no)});
            return *yesFlow && *noFlow;
        }
        if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
            const auto value = expression(ret->getRetValue());
            if (!value)
                return std::nullopt;
            if (ir_.values[*value].type != ir_.returnType ||
                ir_.values[*value].recordName != ir_.returnRecord) {
                error(ret->getReturnLoc(), "return value must match the declared result type");
                return std::nullopt;
            }
            append(Return{*value});
            return true;
        }
        if (const auto *failure = llvm::dyn_cast<clang::CXXThrowExpr>(statement)) {
            const auto value = expression(failure->getSubExpr());
            if (!value)
                return std::nullopt;
            const auto &type = ir_.values[*value];
            if (type.type != Type::Record ||
                (!ir_.errorRecord.empty() && ir_.errorRecord != type.recordName)) {
                error(failure->getThrowLoc(),
                      "typed errors must use one flat record type per operation");
                return std::nullopt;
            }
            ir_.errorRecord = type.recordName;
            append(Return{*value, true});
            return true;
        }
        if (const auto *declarations = llvm::dyn_cast<clang::DeclStmt>(statement)) {
            for (const auto *decl : declarations->decls()) {
                const auto *local = llvm::dyn_cast<clang::VarDecl>(decl);
                const auto type =
                    local ? inputType(local->getType().getUnqualifiedType()) : std::nullopt;
                if (!local || !type || !local->hasInit() ||
                    local->getStorageClass() != clang::SC_None || local->hasAttrs() ||
                    local->isConstexpr() || local->getType().isVolatileQualified() ||
                    local->getType().isRestrictQualified()) {
                    error(decl->getLocation(),
                          "locals must be initialized supported values, optionally const");
                    return std::nullopt;
                }
                const auto value = expression(local->getInit());
                if (!value)
                    return std::nullopt;
                bindings_.emplace(local,
                                  add(Reference{*value, local->getNameAsString()},
                                      ir_.values[*value].type, ir_.values[*value].recordName));
            }
            return false;
        }
        if (const auto *assignment = llvm::dyn_cast<clang::BinaryOperator>(statement);
            assignment && assignment->getOpcode() == clang::BO_Assign) {
            const auto rhs = expression(assignment->getRHS());
            if (!rhs)
                return std::nullopt;
            const auto *lhs = assignment->getLHS()->IgnoreParens();
            if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(lhs)) {
                const auto binding = bindings_.find(ref->getDecl());
                if (binding != bindings_.end() &&
                    (llvm::isa<clang::VarDecl>(ref->getDecl()) &&
                     !llvm::isa<clang::ParmVarDecl>(ref->getDecl()))) {
                    append(Store{binding->second, *rhs});
                    return false;
                }
            }
            if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(lhs);
                member && !member->isArrow()) {
                const auto *base =
                    llvm::dyn_cast<clang::DeclRefExpr>(member->getBase()->IgnoreParenImpCasts());
                const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
                if (base && field && bindings_.contains(base->getDecl()) &&
                    (base->getDecl() == stateDecl_ ||
                     (llvm::isa<clang::VarDecl>(base->getDecl()) &&
                      !llvm::isa<clang::ParmVarDecl>(base->getDecl())))) {
                    append(
                        FieldStore{bindings_.at(base->getDecl()), field->getNameAsString(), *rhs});
                    return false;
                }
            }
            error(assignment->getOperatorLoc(), "assignment requires a local variable or a state "
                                                "field; event inputs are read-only");
            return std::nullopt;
        }
        error(statement->getBeginLoc(),
              std::format("unsupported statement: {}", statement->getStmtClassName()));
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Region> branch(const clang::Expr *expr) {
        Region region;
        auto *outer = std::exchange(currentRegion_, &region);
        const auto result = expression(expr);
        currentRegion_ = outer;
        if (!result)
            return std::nullopt;
        region.result = *result;
        return region;
    }
    [[nodiscard]] std::optional<ValueId> expression(const clang::Expr *expr) {
        if (!expr)
            return std::nullopt; // Invalid C++ has already been diagnosed.
        if (expr->getExprLoc().isMacroID()) {
            error(expr->getExprLoc(), "macro expansions are not supported");
            return std::nullopt;
        }
        if (const auto *parens = llvm::dyn_cast<clang::ParenExpr>(expr)) {
            return expression(parens->getSubExpr());
        }
        if (headers_.options.objects) {
            if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(expr))
                return expression(cleanups->getSubExpr());
            if (const auto *materialize = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr))
                return expression(materialize->getSubExpr());
            if (const auto *cast = llvm::dyn_cast<clang::CXXFunctionalCastExpr>(expr);
                cast && cast->getCastKind() == clang::CK_NoOp &&
                inputType(cast->getType()) == Type::Record)
                return expression(cast->getSubExpr());
            if (const auto *zero = llvm::dyn_cast<clang::ImplicitValueInitExpr>(expr)) {
                const auto type = inputType(zero->getType());
                if (type == Type::Double)
                    return add(Constant{0.0});
                if (type == Type::Int)
                    return add(IntegerConstant{0}, Type::Int);
                if (type == Type::Bool)
                    return add(BooleanConstant{false}, Type::Bool);
            }
            if (const auto *init = llvm::dyn_cast<clang::InitListExpr>(expr);
                init && inputType(init->getType()) == Type::Record) {
                const auto *semantic = init->isSemanticForm() ? init : init->getSemanticForm();
                if (!semantic)
                    semantic = init;
                std::vector<ValueId> fields;
                for (const auto *field : semantic->inits()) {
                    const auto value = expression(field);
                    if (!value)
                        return std::nullopt;
                    fields.push_back(*value);
                }
                return add(RecordInit{std::move(fields)}, Type::Record,
                           recordName(init->getType()));
            }
            if (const auto *copy = llvm::dyn_cast<clang::CXXConstructExpr>(expr);
                copy && copy->getConstructor()->isCopyOrMoveConstructor() &&
                copy->getConstructor()->isTrivial() && copy->getNumArgs() == 1 &&
                inputType(copy->getType()) == Type::Record)
                return expression(copy->getArg(0));
            if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
                const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
                if (!field || member->isArrow() || !inputType(field->getType())) {
                    error(member->getExprLoc(), "only flat event field reads are supported");
                    return std::nullopt;
                }
                const auto base = expression(member->getBase());
                if (!base)
                    return std::nullopt;
                return add(Member{.base = *base, .field = field->getNameAsString()},
                           *inputType(field->getType()), recordName(field->getType()));
            }
            if (const auto *integer = llvm::dyn_cast<clang::IntegerLiteral>(expr);
                integer && inputType(integer->getType()) == Type::Int)
                return add(
                    IntegerConstant{.value = static_cast<int>(integer->getValue().getSExtValue())},
                    Type::Int);
        }
        if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
            if (cast->getCastKind() == clang::CK_LValueToRValue && inputType(cast->getType())) {
                return expression(cast->getSubExpr());
            }
            if (headers_.options.objects && cast->getCastKind() == clang::CK_IntegralToFloating &&
                plainDouble(cast->getType())) {
                // Permit exact int literal promotion, not arbitrary numeric conversions.
                if (const auto *literal =
                        llvm::dyn_cast<clang::IntegerLiteral>(cast->getSubExpr()->IgnoreParens());
                    literal && inputType(literal->getType()) == Type::Int)
                    return add(
                        Constant{.value = static_cast<double>(literal->getValue().getSExtValue())});
            }
            if (headers_.options.objects && cast->getCastKind() == clang::CK_NoOp &&
                inputType(cast->getType().getUnqualifiedType()) == Type::Record &&
                compiler_.getASTContext().hasSameUnqualifiedType(cast->getType(),
                                                                 cast->getSubExpr()->getType()))
                return expression(cast->getSubExpr());
            // C++23 implicit move in return adds an lvalue-to-xvalue NoOp.
            // Unwrap unchanged values only; this is not a numeric cast.
            const auto *operand = cast->getSubExpr();
            if (cast->getCastKind() == clang::CK_NoOp && cast->isXValue() && operand->isLValue() &&
                cast->getType() == operand->getType() &&
                (cast->getType()->isSpecificBuiltinType(clang::BuiltinType::Double) ||
                 (headers_.options.objects && inputType(cast->getType().getUnqualifiedType()))) &&
                llvm::isa<clang::DeclRefExpr>(operand->IgnoreParens())) {
                return expression(operand);
            }
            error(expr->getExprLoc(),
                  "implicit conversions are not supported; use double literals and values");
            return std::nullopt;
        }
        if (const auto *boolean = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(expr)) {
            return add(BooleanConstant{.value = boolean->getValue()}, Type::Bool);
        }
        if (const auto *select = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
            const auto condition = expression(select->getCond());
            if (!condition)
                return std::nullopt;
            if (ir_.values[*condition].type != Type::Bool) {
                error(select->getQuestionLoc(), "conditional requires a bool condition");
                return std::nullopt;
            }
            auto whenTrue = branch(select->getTrueExpr());
            if (!whenTrue)
                return std::nullopt;
            auto whenFalse = branch(select->getFalseExpr());
            if (!whenFalse)
                return std::nullopt;
            const auto type = ir_.values[whenTrue->result].type;
            if (type != ir_.values[whenFalse->result].type) {
                error(select->getQuestionLoc(), "conditional branches must have the same type");
                return std::nullopt;
            }
            return add(Select{.condition = *condition,
                              .whenTrue = std::move(*whenTrue),
                              .whenFalse = std::move(*whenFalse)},
                       type, recordName(select->getType()));
        }
        if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr)) {
            const auto *callee = call->getDirectCallee();
            if (!callee) {
                error(call->getExprLoc(),
                      "only direct runtime or DSL function calls are supported");
                return std::nullopt;
            }
            const auto *canonical = callee->getCanonicalDecl();
            std::optional<CallTarget> target;
            std::size_t arity = 0;
            if (const auto builtin = builtinDeclarations_.find(canonical);
                builtin != builtinDeclarations_.end()) {
                target = builtin->second;
                arity = mathBuiltin(builtin->second).arity;
            } else if (const auto function = dslDeclarations_.find(canonical);
                       function != dslDeclarations_.end()) {
                target = DslFunction{.id = function->second};
                arity = definitions_[function->second]->getNumParams();
            } else if (const auto external = externalDeclarations_.find(canonical);
                       external != externalDeclarations_.end()) {
                target = ExternalFunction{.id = external->second};
                arity = module_.externals[external->second].arity;
            } else {
                error(call->getExprLoc(), "callee must be a runtime API, a defined DSL function, "
                                          "or an authorized external function");
                return std::nullopt;
            }
            if (call->getNumArgs() != arity) {
                error(call->getExprLoc(), "incorrect number of function arguments");
                return std::nullopt;
            }
            std::vector<ValueId> arguments;
            for (const auto [index, argument] : call->arguments() | std::views::enumerate) {
                const auto value = expression(argument);
                if (!value)
                    return std::nullopt;
                const auto expected =
                    headers_.options.objects
                        ? inputType(
                              callee->getParamDecl(index)->getOriginalType().getNonReferenceType())
                        : std::optional{Type::Double};
                if (!expected || ir_.values[*value].type != *expected) {
                    error(
                        argument->getExprLoc(),
                        "function arguments must match the declared type (double in scalar mode)");
                    return std::nullopt;
                }
                if (callee->getParamDecl(index)->getOriginalType()->isReferenceType() &&
                    (!ir_.stateParameter || *value != *ir_.stateParameter)) {
                    error(argument->getExprLoc(),
                          "a stateful DSL call must receive the caller's explicit state parameter");
                    return std::nullopt;
                }
                // A by-value argument is a snapshot, even when the callee also
                // receives and updates the same record as its explicit state.
                if (headers_.options.objects && std::holds_alternative<DslFunction>(*target) &&
                    !callee->getParamDecl(index)->getOriginalType()->isReferenceType())
                    arguments.push_back(add(Reference{*value, ""}, ir_.values[*value].type,
                                            ir_.values[*value].recordName));
                else
                    arguments.push_back(*value);
            }
            return add(Call{.target = *target,
                            .arguments = std::move(arguments),
                            .location = sourcePosition(call->getExprLoc())},
                       *inputType(call->getType()), recordName(call->getType()));
        }
        if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
            if (headers_.options.objects && unary->getOpcode() == clang::UO_LNot) {
                const auto operand = expression(unary->getSubExpr());
                if (!operand)
                    return std::nullopt;
                if (ir_.values[*operand].type != Type::Bool) {
                    error(unary->getOperatorLoc(), "logical ! requires bool");
                    return std::nullopt;
                }
                return add(Unary{UnaryOp::Not, *operand}, Type::Bool);
            }
            if (unary->getOpcode() != clang::UO_Plus && unary->getOpcode() != clang::UO_Minus) {
                error(unary->getOperatorLoc(), "only unary + and - are supported");
                return std::nullopt;
            }
            const auto operand = expression(unary->getSubExpr());
            if (!operand)
                return std::nullopt;
            if (ir_.values[*operand].type != Type::Double) {
                error(unary->getOperatorLoc(), "unary arithmetic requires double");
                return std::nullopt;
            }
            return add(
                Unary{.op = unary->getOpcode() == clang::UO_Plus ? UnaryOp::Plus : UnaryOp::Negate,
                      .operand = *operand});
        }
        if (const auto *literal = llvm::dyn_cast<clang::FloatingLiteral>(expr)) {
            if (!plainDouble(literal->getType()) || !literal->getValue().isFinite()) {
                error(expr->getExprLoc(), "only finite double literals are supported");
                return std::nullopt;
            }
            return add(Constant{.value = literal->getValue().convertToDouble()});
        }
        if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
            const auto binding = bindings_.find(ref->getDecl());
            if (binding == bindings_.end()) {
                error(
                    expr->getExprLoc(),
                    "reference must name a parameter or an already initialized const double local");
                return std::nullopt;
            }
            return binding->second;
        }
        if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
            if (headers_.options.objects &&
                (binary->getOpcode() == clang::BO_LAnd || binary->getOpcode() == clang::BO_LOr)) {
                const auto lhs = expression(binary->getLHS());
                if (!lhs)
                    return std::nullopt;
                auto rhs = branch(binary->getRHS());
                if (!rhs)
                    return std::nullopt;
                if (ir_.values[*lhs].type != Type::Bool ||
                    ir_.values[rhs->result].type != Type::Bool) {
                    error(binary->getOperatorLoc(), "logical operators require bool operands");
                    return std::nullopt;
                }
                Region constant;
                auto *outer = std::exchange(currentRegion_, &constant);
                constant.result =
                    add(BooleanConstant{binary->getOpcode() == clang::BO_LOr}, Type::Bool);
                currentRegion_ = outer;
                if (binary->getOpcode() == clang::BO_LAnd)
                    return add(Select{*lhs, std::move(*rhs), std::move(constant)}, Type::Bool);
                return add(Select{*lhs, std::move(constant), std::move(*rhs)}, Type::Bool);
            }
            BinaryOp op;
            switch (binary->getOpcode()) {
            case clang::BO_Add:
                op = BinaryOp::Add;
                break;
            case clang::BO_Sub:
                op = BinaryOp::Subtract;
                break;
            case clang::BO_Mul:
                op = BinaryOp::Multiply;
                break;
            case clang::BO_Div:
                op = BinaryOp::Divide;
                break;
            case clang::BO_LT:
                op = BinaryOp::Less;
                break;
            case clang::BO_LE:
                op = BinaryOp::LessEqual;
                break;
            case clang::BO_GT:
                op = BinaryOp::Greater;
                break;
            case clang::BO_GE:
                op = BinaryOp::GreaterEqual;
                break;
            case clang::BO_EQ:
                op = BinaryOp::Equal;
                break;
            case clang::BO_NE:
                op = BinaryOp::NotEqual;
                break;
            default:
                error(binary->getOperatorLoc(),
                      "only binary arithmetic and comparisons are supported");
                return std::nullopt;
            }
            const auto lhs = expression(binary->getLHS());
            if (!lhs)
                return std::nullopt;
            const auto rhs = expression(binary->getRHS());
            if (!rhs)
                return std::nullopt;
            const bool integerComparison = headers_.options.objects && binary->isComparisonOp() &&
                                           ir_.values[*lhs].type == Type::Int &&
                                           ir_.values[*rhs].type == Type::Int;
            if (!integerComparison &&
                (ir_.values[*lhs].type != Type::Double || ir_.values[*rhs].type != Type::Double)) {
                error(binary->getOperatorLoc(),
                      "binary arithmetic and comparisons require double operands");
                return std::nullopt;
            }
            return add(Binary{.op = op, .lhs = *lhs, .rhs = *rhs},
                       binary->isComparisonOp() ? Type::Bool : Type::Double);
        }
        error(expr->getExprLoc(),
              std::format("unsupported expression: {}", expr->getStmtClassName()));
        return std::nullopt;
    }
    clang::CompilerInstance &compiler_;
    std::optional<Module> &result_;
    HeaderPolicy &headers_;
    std::unordered_map<const clang::FunctionDecl *, std::size_t> externalDeclarations_;
    std::unordered_map<std::string, const clang::FunctionDecl *> externalNames_;
    Module module_;
    Function ir_;
    std::vector<const clang::FunctionDecl *> definitions_;
    std::unordered_map<const clang::FunctionDecl *, FunctionId> dslDeclarations_;
    Region *currentRegion_ = &ir_.body;
    std::unordered_map<const clang::FunctionDecl *, MathFunction> builtinDeclarations_;
    std::unordered_map<const clang::ValueDecl *, ValueId> bindings_;
    const clang::ParmVarDecl *stateDecl_ = nullptr;
};

class Action final : public clang::ASTFrontendAction {
  public:
    Action(std::optional<Module> &result, HeaderPolicy &headers)
        : result_(result), headers_(headers) {}
    bool BeginSourceFileAction(clang::CompilerInstance &compiler) override {
        if (!checkTokens(compiler, headers_))
            return false;
        compiler.getPreprocessor().addPPCallbacks(
            std::make_unique<MacroChecker>(compiler, headers_));
        return true;
    }
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &compiler,
                                                          llvm::StringRef) override {
        return std::make_unique<Lowering>(compiler, result_, headers_);
    }

  private:
    std::optional<Module> &result_;
    HeaderPolicy &headers_;
};
} // namespace

std::optional<Module> compile(std::string_view source, std::string_view filename,
                              const CompileOptions &options) {
    std::optional<Module> result;
    HeaderPolicy headers{.options = options, .included = {}, .macros = {}};
    std::vector<std::string> arguments = {"-xc++",
                                          "-std=c++23",
                                          "-pedantic-errors",
                                          "-Werror",
                                          "-fno-fast-math",
                                          "-ffp-contract=off",
                                          "-fno-color-diagnostics",
                                          "-fno-builtin",
                                          "-nostdinc",
                                          "-nostdinc++",
                                          "-I",
                                          DSL_RUNTIME_INCLUDE_DIR};
    for (const auto &header : options.externalHeaders) {
        arguments.push_back("-I");
        arguments.push_back(std::filesystem::path(header).parent_path().string());
    }
    for (const auto &library : options.dslLibraries) {
        arguments.push_back("-I");
        arguments.push_back(std::filesystem::path(library).parent_path().string());
    }
    const bool success = clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<Action>(result, headers), llvm::StringRef(source), arguments,
        llvm::StringRef(filename), "dslc");
    if (!success)
        return std::nullopt;
    return result;
}
} // namespace dsl
