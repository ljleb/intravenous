#pragma once

#include <llvm/IR/Function.h>

#include <cstddef>
#include <cstdint>

namespace iv::graph_jit {

// The compiler ABI of one package callback. The package finalizer validates
// each callback definition against this type before emitting node code, and
// GraphJit uses the same type when it declares the imported native address.
enum class CallbackImportAbi : std::uint8_t {
    block,
    background,
    batch,
    sequential_initialize,
    sequential_combine,
};

inline llvm::FunctionType* package_callback_type(
    llvm::LLVMContext& context,
    CallbackImportAbi abi)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* boolean = llvm::Type::getInt1Ty(context);
    auto* byte = llvm::Type::getInt8Ty(context);
    switch (abi) {
    case CallbackImportAbi::block:
        return llvm::FunctionType::get(
            llvm::Type::getVoidTy(context),
            {pointer, pointer, size_type, size_type},
            false);
    case CallbackImportAbi::background:
        return llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), {pointer, pointer}, false);
    case CallbackImportAbi::batch:
        return llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), {pointer}, false);
    case CallbackImportAbi::sequential_initialize:
        return llvm::FunctionType::get(
            boolean, {size_type, byte, byte, pointer, size_type, size_type}, false);
    case CallbackImportAbi::sequential_combine:
        return llvm::FunctionType::get(
            boolean, {size_type, byte, pointer, pointer, size_type, size_type}, false);
    }
    return nullptr;
}

inline bool package_callback_type_matches(
    llvm::Function const& function,
    CallbackImportAbi abi)
{
    return function.getCallingConv() == llvm::CallingConv::C
        && function.getFunctionType()
            == package_callback_type(function.getContext(), abi);
}

} // namespace iv::graph_jit
