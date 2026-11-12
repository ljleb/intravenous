#include <intravenous/graph_jit/region_specialization_emit.h>

#include <gtest/gtest.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// A function with the specialization entry's workspace argument (index 5)
// that reads two 16-byte workspace records, as a loop over a step's
// bindings does when unrolling keeps the loop. join receives the record
// index as a phi of 0 and 1. Each loaded value is stored to out, in order:
//   same    record[index].second, through a phi index
//   differ  record[index].first, through a phi index
//   chosen  record[choice].first, through a select index
//   narrow  record[narrow].second, through an i32 phi index
struct RecordReader {
    llvm::LLVMContext context{};
    std::unique_ptr<llvm::Module> module{};
    llvm::Function* function = nullptr;
    llvm::BasicBlock* first = nullptr;
    llvm::BasicBlock* second = nullptr;
    llvm::BasicBlock* join = nullptr;

    RecordReader()
        : module(std::make_unique<llvm::Module>("iv.substitution.test", context))
    {
        auto* i1 = llvm::Type::getInt1Ty(context);
        auto* i8 = llvm::Type::getInt8Ty(context);
        auto* i32 = llvm::Type::getInt32Ty(context);
        auto* i64 = llvm::Type::getInt64Ty(context);
        auto* pointer = llvm::PointerType::get(context, 0);
        auto* type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context),
            {pointer, pointer, pointer, i64, i64, pointer, i1, pointer},
            false);
        function = llvm::Function::Create(
            type, llvm::GlobalValue::ExternalLinkage, "entry_point", *module);
        auto* workspace = function->getArg(5);
        auto* condition = function->getArg(6);
        auto* out = function->getArg(7);

        auto* entry = llvm::BasicBlock::Create(context, "entry", function);
        first = llvm::BasicBlock::Create(context, "first", function);
        second = llvm::BasicBlock::Create(context, "second", function);
        join = llvm::BasicBlock::Create(context, "join", function);
        llvm::IRBuilder<> builder(entry);
        builder.CreateCondBr(condition, first, second);
        builder.SetInsertPoint(first);
        builder.CreateBr(join);
        builder.SetInsertPoint(second);
        builder.CreateBr(join);

        builder.SetInsertPoint(join);
        auto* record_type = llvm::ArrayType::get(i8, 16);
        auto* index = builder.CreatePHI(i64, 2, "index");
        index->addIncoming(builder.getInt64(0), first);
        index->addIncoming(builder.getInt64(1), second);
        auto* narrow_index = builder.CreatePHI(i32, 2, "narrow.index");
        narrow_index->addIncoming(builder.getInt32(0), first);
        narrow_index->addIncoming(builder.getInt32(1), second);
        auto* record = builder.CreateInBoundsGEP(record_type, workspace, {index}, "record");
        auto* same = builder.CreateLoad(
            i64, builder.CreateConstInBoundsGEP1_64(i8, record, 8), "same");
        auto* differ = builder.CreateLoad(i64, record, "differ");
        auto* choice = builder.CreateSelect(
            condition, builder.getInt64(0), builder.getInt64(1), "choice");
        auto* chosen = builder.CreateLoad(
            i64,
            builder.CreateInBoundsGEP(record_type, workspace, {choice}),
            "chosen");
        auto* narrow = builder.CreateLoad(
            i64,
            builder.CreateConstInBoundsGEP1_64(
                i8,
                builder.CreateInBoundsGEP(record_type, workspace, {narrow_index}),
                8),
            "narrow");
        std::array<llvm::Value*, 4> const results{same, differ, chosen, narrow};
        for (std::size_t slot = 0; slot < results.size(); ++slot) {
            builder.CreateStore(
                results[slot],
                builder.CreateConstInBoundsGEP1_64(i64, out, slot));
        }
        builder.CreateRetVoid();
    }

    // The value stored to out[slot].
    llvm::Value* stored(std::size_t slot) const
    {
        std::size_t seen = 0;
        for (auto& instruction : *join) {
            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                if (seen++ == slot) return store->getValueOperand();
            }
        }
        return nullptr;
    }
};

// Two records whose first fields differ (10 and 20) and whose second fields
// agree (7). Every byte is known.
iv::graph_jit::RegionSpecializationWorkspaceContents record_contents()
{
    iv::graph_jit::RegionSpecializationWorkspaceContents contents;
    contents.bytes.resize(32);
    contents.known.assign(32, 1);
    std::array<std::uint64_t, 4> const fields{10, 7, 20, 7};
    std::memcpy(contents.bytes.data(), fields.data(), sizeof(fields));
    return contents;
}

std::uint64_t constant_value(llvm::Value* value)
{
    auto* constant = llvm::dyn_cast_if_present<llvm::ConstantInt>(value);
    EXPECT_NE(constant, nullptr);
    return constant ? constant->getZExtValue() : 0;
}

} // namespace

TEST(RegionSpecializationSubstitution, ResolvesFieldsAtPhiAndSelectIndexedAddresses)
{
    RecordReader reader;
    auto const replaced = iv::graph_jit::substitute_region_workspace_values(
        *reader.module, "entry_point", record_contents());
    EXPECT_EQ(replaced, 4u);
    std::string errors;
    llvm::raw_string_ostream stream(errors);
    EXPECT_FALSE(llvm::verifyFunction(*reader.function, &stream)) << errors;

    // Every address the phi can form holds 7.
    EXPECT_EQ(constant_value(reader.stored(0)), 7u);

    // The first fields differ, so the load becomes a phi of them on the
    // index's incoming edges.
    auto* differ = llvm::dyn_cast_if_present<llvm::PHINode>(reader.stored(1));
    ASSERT_NE(differ, nullptr);
    ASSERT_EQ(differ->getNumIncomingValues(), 2u);
    EXPECT_EQ(differ->getParent(), reader.join);
    EXPECT_EQ(constant_value(differ->getIncomingValueForBlock(reader.first)), 10u);
    EXPECT_EQ(constant_value(differ->getIncomingValueForBlock(reader.second)), 20u);

    // A select index becomes a select of the fields on its condition.
    auto* chosen = llvm::dyn_cast_if_present<llvm::SelectInst>(reader.stored(2));
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->getCondition(), reader.function->getArg(6));
    EXPECT_EQ(constant_value(chosen->getTrueValue()), 10u);
    EXPECT_EQ(constant_value(chosen->getFalseValue()), 20u);

    // An i32 index is sign-extended to the index width.
    EXPECT_EQ(constant_value(reader.stored(3)), 7u);
}

TEST(RegionSpecializationSubstitution, KeepsLoadsWhoseAddressesCanReachUnknownBytes)
{
    // The second record's first field is unknown, as a mutable binding
    // field or a runtime storage pointer is.
    auto contents = record_contents();
    std::fill_n(contents.known.begin() + 16, 8, std::uint8_t{0});

    RecordReader reader;
    auto const replaced = iv::graph_jit::substitute_region_workspace_values(
        *reader.module, "entry_point", contents);
    // Only the second fields, which both records still know, are replaced.
    EXPECT_EQ(replaced, 2u);
    EXPECT_EQ(constant_value(reader.stored(0)), 7u);
    EXPECT_TRUE(llvm::isa<llvm::LoadInst>(reader.stored(1)));
    EXPECT_TRUE(llvm::isa<llvm::LoadInst>(reader.stored(2)));
    EXPECT_EQ(constant_value(reader.stored(3)), 7u);
}

TEST(RegionSpecializationTarget, RetargetsDefinitionsAndPreservesExtraFeatures)
{
    llvm::LLVMContext context;
    llvm::Module module("iv.region.target.test", context);
    auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
    auto* imported = llvm::Function::Create(
        type, llvm::GlobalValue::InternalLinkage, "imported", module);
    llvm::BasicBlock::Create(context, "entry", imported);
    llvm::IRBuilder<> imported_builder(&imported->getEntryBlock());
    imported_builder.CreateRetVoid();
    imported->addFnAttr("target-cpu", "x86-64");
    imported->addFnAttr("target-features", "+sse2");
    imported->addFnAttr("tune-cpu", "generic");

    auto* generated = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "generated", module);
    llvm::BasicBlock::Create(context, "entry", generated);
    llvm::IRBuilder<> generated_builder(&generated->getEntryBlock());
    generated_builder.CreateRetVoid();

    auto* declaration = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "external", module);
    declaration->addFnAttr("target-cpu", "external-cpu");

    EXPECT_EQ(
        iv::graph_jit::retarget_region_specialization_functions(
            module, "host-cpu", "+avx2,+fma"),
        2u);
    EXPECT_EQ(
        imported->getFnAttribute("target-cpu").getValueAsString(),
        "host-cpu");
    EXPECT_EQ(
        imported->getFnAttribute("target-features").getValueAsString(),
        "+avx2,+fma,+sse2");
    EXPECT_FALSE(imported->hasFnAttribute("tune-cpu"));
    EXPECT_EQ(
        generated->getFnAttribute("target-cpu").getValueAsString(),
        "host-cpu");
    EXPECT_EQ(
        generated->getFnAttribute("target-features").getValueAsString(),
        "+avx2,+fma");
    EXPECT_EQ(
        declaration->getFnAttribute("target-cpu").getValueAsString(),
        "external-cpu");
}
