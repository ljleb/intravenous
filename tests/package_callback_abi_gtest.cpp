#include <intravenous/graph_jit/package_callback_abi.h>

#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>

#include <array>
#include <memory>

TEST(PackageCallbackAbi, AcceptsOnlyTheDeclaredFunctionTypeAndCallingConvention)
{
    using iv::graph_jit::CallbackImportAbi;
    constexpr std::array callback_abis{
        CallbackImportAbi::block,
        CallbackImportAbi::background,
        CallbackImportAbi::batch,
        CallbackImportAbi::sequential_initialize,
        CallbackImportAbi::sequential_combine,
    };

    llvm::LLVMContext context;
    auto* wrong_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {}, false);
    for (auto const abi : callback_abis) {
        auto exact = std::unique_ptr<llvm::Function>(llvm::Function::Create(
            iv::graph_jit::package_callback_type(context, abi),
            llvm::GlobalValue::ExternalLinkage));
        EXPECT_TRUE(iv::graph_jit::package_callback_type_matches(*exact, abi));

        exact->setCallingConv(llvm::CallingConv::Fast);
        EXPECT_FALSE(iv::graph_jit::package_callback_type_matches(*exact, abi));

        auto wrong = std::unique_ptr<llvm::Function>(llvm::Function::Create(
            wrong_type, llvm::GlobalValue::ExternalLinkage));
        EXPECT_FALSE(iv::graph_jit::package_callback_type_matches(*wrong, abi));
    }
}
