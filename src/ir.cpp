#include "dsl/ir.h"
#include "dsl/registry.h"
#include <format>
namespace dsl::ir {
namespace {
std::string values(const std::vector<ValueId> &ids) {
    std::string result;
    for (auto id : ids) {
        if (!result.empty())
            result += ", ";
        result += std::format("%{}", id.value);
    }
    return result;
}
std::string region(const Region &body, const std::string &indent) {
    std::string text;
    for (const auto &op : body.operations) {
        const auto *definition = lookup(op.code, registry());
        text += indent;
        if (!op.results.empty())
            text += values(op.results) + " = ";
        text += definition ? definition->name : "<unknown>";
        if (!op.operands.empty())
            text += " " + values(op.operands);
        if (const auto *id = std::get_if<ConstantId>(&op.attribute))
            text += std::format(" #c{}", id->value);
        if (const auto *id = std::get_if<FieldId>(&op.attribute))
            text += std::format(" #field{}", id->value);
        if (const auto *id = std::get_if<FunctionId>(&op.attribute))
            text += std::format(" @f{}", id->value);
        if (const auto *id = std::get_if<ExternalId>(&op.attribute))
            text += std::format(" @external{}", id->value);
        text += '\n';
        for (const auto &child : op.regions)
            text += indent + "{\n" + region(child, indent + "  ") + indent + "}\n";
    }
    if (!body.terminator)
        return text + indent + "<missing terminator>\n";
    if (const auto *t = std::get_if<ReturnSuccess>(&*body.terminator))
        text += indent + "return_success " + values(t->values) + "\n";
    else if (const auto *t = std::get_if<ReturnError>(&*body.terminator))
        text += indent + std::format("return_error %{}\n", t->error.value);
    else if (const auto *t = std::get_if<Yield>(&*body.terminator))
        text += indent + "yield " + values(t->values) + "\n";
    else
        text += indent + "unreachable\n";
    return text;
}
} // namespace
std::string format(const Module &module) {
    std::string text = "computation_ir v1\n";
    for (std::size_t i = 0; i < module.types.size(); ++i) {
        text += std::format("type !{} = ", i);
        const auto &t = module.types[i];
        if (std::holds_alternative<BoolType>(t))
            text += "bool";
        else if (const auto *integer = std::get_if<IntegerType>(&t))
            text += std::format("{}{}", integer->signedness == Signedness::Signed ? "i" : "u",
                                integer->width);
        else if (const auto *fp = std::get_if<FloatType>(&t))
            text += fp->format == FloatFormat::IEEE754Binary64 ? "ieee754.binary64"
                                                               : "ieee754.binary32";
        else {
            text += "record {";
            for (const auto &f : std::get<RecordType>(t).fields)
                text += std::format(" #{}: !{}", f.id.value, f.type.value);
            text += " }";
        }
        text += '\n';
    }
    for (std::size_t i = 0; i < module.constants.size(); ++i) {
        const auto &c = module.constants[i];
        text += std::format("constant #c{} : !{} = ", i, c.type.value);
        if (const auto *bits = std::get_if<std::uint64_t>(&c.payload))
            text += std::format("bits 0x{:x}", *bits);
        else {
            text += "{";
            for (auto id : std::get<std::vector<ConstantId>>(c.payload))
                text += std::format(" #c{}", id.value);
            text += " }";
        }
        text += '\n';
    }
    for (std::size_t i = 0; i < module.externals.size(); ++i)
        text += std::format("external @external{} // {}\n", i, module.externals[i].debugName);
    for (const auto &f : module.functions) {
        text += "func @" + f.debugName + " (";
        for (auto id : f.parameters)
            text += std::format(" %{}: !{}", id.value, f.values[id.value].value);
        text += " ) -> (";
        for (auto t : f.signature.results)
            text += std::format(" !{}", t.value);
        text += " )";
        if (f.signature.error)
            text += std::format(" error !{}", f.signature.error->value);
        text += " {\n";
        for (std::size_t i = 0; i < f.values.size(); ++i)
            text += std::format("  value %{} : !{}\n", i, f.values[i].value);
        text += region(f.body, "  ") + "}\n";
    }
    return text;
}
} // namespace dsl::ir
