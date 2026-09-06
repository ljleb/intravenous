#include <intravenous/graph/builder.h>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/module/authored_graph_wire.h>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using namespace llvm;

struct Options {
    std::filesystem::path metadata_dir;
    bool optimize = true;
    std::vector<std::string> link_command;
};

[[noreturn]] void fail(std::string const& message)
{
    throw std::runtime_error("iv-module-finalize: " + message);
}

std::string error_string(Error error)
{
    std::string result;
    raw_string_ostream stream(result);
    logAllUnhandledErrors(std::move(error), stream);
    stream.flush();
    return result;
}

template<class T>
T take_expected(Expected<T> value, std::string_view context)
{
    if (!value) fail(std::string(context) + ": " + error_string(value.takeError()));
    return std::move(*value);
}

void check_error(Error error, std::string_view context)
{
    if (error) fail(std::string(context) + ": " + error_string(std::move(error)));
}

Options parse_options(int argc, char** argv)
{
    Options result;
    bool command = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (!command && arg == "--") {
            command = true;
            continue;
        }
        if (!command && arg.starts_with("--metadata-dir=")) {
            result.metadata_dir = std::string(arg.substr(std::string_view("--metadata-dir=").size()));
            continue;
        }
        if (!command && arg.starts_with("--optimization=")) {
            auto const value = arg.substr(std::string_view("--optimization=").size());
            if (value == "O0") result.optimize = false;
            else if (value == "O3") result.optimize = true;
            else fail("unknown optimization level '" + std::string(value) + "'");
            continue;
        }
        if (!command) fail("unknown launcher option '" + std::string(arg) + "'");
        result.link_command.emplace_back(arg);
    }
    if (result.metadata_dir.empty()) fail("missing --metadata-dir");
    if (result.link_command.empty()) fail("missing compiler link command after --");
    return result;
}

std::vector<std::string> expand_response_files(std::vector<std::string> args)
{
    BumpPtrAllocator allocator;
    StringSaver saver(allocator);
    SmallVector<const char*, 64> raw;
    for (auto& arg : args) raw.push_back(saver.save(arg).data());
    cl::ExpandResponseFiles(saver, cl::TokenizeGNUCommandLine, raw);
    std::vector<std::string> result;
    result.reserve(raw.size());
    for (auto* arg : raw) result.emplace_back(arg);
    return result;
}

bool try_parse_bitcode(
    std::filesystem::path const& path,
    LLVMContext& context,
    std::unique_ptr<Module>& output)
{
    auto buffer = MemoryBuffer::getFile(path.string());
    if (!buffer) return false;
    auto module = parseBitcodeFile((*buffer)->getMemBufferRef(), context);
    if (!module) {
        consumeError(module.takeError());
        return false;
    }
    output = std::move(*module);
    return true;
}

struct LinkedModule {
    std::unique_ptr<Module> module;
    std::vector<std::filesystem::path> bitcode_inputs;
};

LinkedModule link_bitcode_inputs(
    std::span<std::string const> command,
    LLVMContext& context)
{
    LinkedModule result;
    for (std::size_t i = 1; i < command.size(); ++i) {
        std::filesystem::path path(command[i]);
        if (command[i].empty() || command[i][0] == '-' || !std::filesystem::is_regular_file(path)) continue;
        std::unique_ptr<Module> input;
        if (!try_parse_bitcode(path, context, input)) continue;
        result.bitcode_inputs.push_back(std::filesystem::absolute(path));
        if (!result.module) {
            result.module = std::move(input);
        } else if (Linker::linkModules(*result.module, std::move(input))) {
            fail("LLVM link failed while combining '" + path.string() + "'");
        }
    }
    if (!result.module) fail("link command contains no LLVM bitcode object inputs");
    return result;
}

struct IrNodeRecord {
    iv::NodeCodeKey key{};
    std::string type_name{};
    Constant* initializer = nullptr;
};

std::uint64_t constant_u64(Value const* value)
{
    auto const* integer = dyn_cast<ConstantInt>(value);
    if (!integer) fail("malformed iv_node_types code key");
    return integer->getZExtValue();
}

std::vector<IrNodeRecord> scan_node_records(Module& module)
{
    std::vector<IrNodeRecord> result;
    for (auto& global : module.globals()) {
        auto const section = global.getSection();
        if (section != "iv_node_types" && !section.ends_with("__iv_node_types")) continue;
        auto* record = dyn_cast_or_null<ConstantStruct>(global.getInitializer());
        if (!record || record->getNumOperands() < 4) fail("malformed iv_node_types record");
        auto* key = dyn_cast<ConstantStruct>(record->getOperand(0));
        if (!key || key->getNumOperands() != 2) fail("malformed iv_node_types key");
        StringRef name;
        if (!getConstantStringInfo(record->getOperand(2), name, 0, false))
            fail("node compiler record does not contain a constant type name");
        auto const name_size = constant_u64(record->getOperand(3));
        if (name_size > name.size()) fail("node compiler record type name length is invalid");
        result.push_back({
            .key = {
                .low = constant_u64(key->getOperand(0)),
                .high = constant_u64(key->getOperand(1)),
            },
            .type_name = name.substr(0, name_size).str(),
            .initializer = record,
        });
    }
    if (result.empty()) fail("module emitted no iv_node_types compiler records");
    std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
        if (a.key.high != b.key.high) return a.key.high < b.key.high;
        return a.key.low < b.key.low;
    });
    auto duplicate = std::adjacent_find(result.begin(), result.end(), [](auto const& a, auto const& b) {
        return a.key == b.key;
    });
    if (duplicate != result.end()) fail("duplicate NodeCodeKey in compiler records");
    return result;
}

std::string normalize_type_name(std::string value)
{
    while (value.starts_with("::")) value.erase(0, 2);
    for (auto const prefix : {std::string_view("struct "), std::string_view("class "), std::string_view("union ")}) {
        if (value.starts_with(prefix)) value.erase(0, prefix.size());
    }
    return value;
}

struct CompilerMetadata {
    std::vector<std::pair<std::string, iv::NodeStateStructure>> states;
};

std::string read_file(std::filesystem::path const& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("cannot read metadata file '" + path.string() + "'");
    return {std::istreambuf_iterator<char>(stream), {}};
}

CompilerMetadata load_metadata(std::filesystem::path const& directory)
{
    CompilerMetadata result;
    if (!std::filesystem::exists(directory)) return result;
    for (auto const& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || !entry.path().filename().string().ends_with(".ivmeta.json")) continue;
        auto parsed = json::parse(read_file(entry.path()));
        if (!parsed) fail("invalid metadata JSON in '" + entry.path().string() + "': " + error_string(parsed.takeError()));
        auto* object = parsed->getAsObject();
        if (!object) fail("metadata root is not an object in '" + entry.path().string() + "'");
        if (auto* states = object->getArray("states")) {
            for (auto const& state_value : *states) {
                auto* state = state_value.getAsObject();
                if (!state) continue;
                auto node_name = state->getString("node_type");
                auto size_bits = state->getInteger("size_bits");
                auto alignment_bits = state->getInteger("alignment_bits");
                auto* fields = state->getArray("fields");
                if (!node_name || !size_bits || !alignment_bits || !fields) continue;
                iv::NodeStateStructure structure{
                    .size_bits = static_cast<std::size_t>(*size_bits),
                    .alignment_bits = static_cast<std::size_t>(*alignment_bits),
                };
                for (auto const& field_value : *fields) {
                    auto* field = field_value.getAsObject();
                    if (!field) continue;
                    auto name = field->getString("name");
                    auto type_usr = field->getString("type_usr");
                    auto type = field->getString("type");
                    auto bit_offset = field->getInteger("bit_offset");
                    auto field_size = field->getInteger("size_bits");
                    auto field_alignment = field->getInteger("alignment_bits");
                    if (!name || (!type_usr && !type) || !bit_offset || !field_size || !field_alignment) continue;
                    iv::NodeStateFieldStructure item{
                        .name = name->str(),
                        .type_name = type_usr ? type_usr->str() : type->str(),
                        .bit_offset = static_cast<std::size_t>(*bit_offset),
                        .size_bits = static_cast<std::size_t>(*field_size),
                        .alignment_bits = static_cast<std::size_t>(*field_alignment),
                    };
                    if (auto width = field->getInteger("bit_width")) item.bit_width = static_cast<std::size_t>(*width);
                    structure.fields.push_back(std::move(item));
                }
                result.states.emplace_back(normalize_type_name(node_name->str()), std::move(structure));
            }
        }
    }
    return result;
}

void mark_reachable(Value const* value, SmallPtrSetImpl<GlobalValue const*>& reachable)
{
    if (!value) return;
    if (auto const* global = dyn_cast<GlobalValue>(value)) {
        if (!reachable.insert(global).second) return;
        if (auto const* function = dyn_cast<Function>(global)) {
            if (!function->isDeclaration()) {
                for (auto const& block : *function)
                    for (auto const& instruction : block)
                        for (auto const& operand : instruction.operands())
                            mark_reachable(operand.get(), reachable);
            }
        } else if (auto const* variable = dyn_cast<GlobalVariable>(global)) {
            if (variable->hasInitializer()) mark_reachable(variable->getInitializer(), reachable);
        } else if (auto const* alias = dyn_cast<GlobalAlias>(global)) {
            mark_reachable(alias->getAliasee(), reachable);
        }
        return;
    }
    if (auto const* constant = dyn_cast<Constant>(value)) {
        for (auto const& operand : constant->operands()) mark_reachable(operand.get(), reachable);
    }
}

std::unique_ptr<Module> clone_authoring_module(Module const& master)
{
    auto const* author = master.getFunction("iv_module_author");
    if (!author || author->isDeclaration()) fail("master LLVM module does not define iv_module_author");

    SmallPtrSet<GlobalValue const*, 32> reachable;
    mark_reachable(author, reachable);
    if (auto const* ctors = master.getGlobalVariable("llvm.global_ctors")) mark_reachable(ctors, reachable);
    if (auto const* dtors = master.getGlobalVariable("llvm.global_dtors")) mark_reachable(dtors, reachable);

    ValueToValueMapTy map;
    return CloneModule(master, map, [&](GlobalValue const* global) {
        if (global->isDeclaration()) return false;
        return reachable.contains(global);
    });
}

std::vector<std::filesystem::path> library_search_paths(
    std::span<std::string const> command)
{
    std::vector<std::filesystem::path> result;
    for (std::size_t i = 1; i < command.size(); ++i) {
        std::string_view arg(command[i]);
        if (arg == "-L" && i + 1 < command.size()) {
            result.emplace_back(command[++i]);
        } else if (arg.starts_with("-L") && arg.size() > 2) {
            result.emplace_back(std::string(arg.substr(2)));
        }
    }
    return result;
}

std::optional<std::filesystem::path> resolve_link_library(
    std::string_view name,
    std::span<std::filesystem::path const> search_paths)
{
#if defined(_WIN32)
    std::array<std::string, 2> names{
        std::string(name) + ".lib", std::string(name) + ".dll"};
#elif defined(__APPLE__)
    std::array<std::string, 3> names{
        "lib" + std::string(name) + ".dylib",
        "lib" + std::string(name) + ".a",
        "lib" + std::string(name) + ".so"};
#else
    std::array<std::string, 2> names{
        "lib" + std::string(name) + ".so",
        "lib" + std::string(name) + ".a"};
#endif
    for (auto const& directory : search_paths) {
        for (auto const& candidate_name : names) {
            auto candidate = directory / candidate_name;
            if (std::filesystem::is_regular_file(candidate)) return candidate;
        }
    }
    return std::nullopt;
}

void add_jit_library(
    orc::LLJIT& jit,
    std::filesystem::path const& path)
{
    auto& dylib = jit.getMainJITDylib();
    auto const extension = path.extension().string();
    if (extension == ".a" || extension == ".lib") {
        if (auto error = jit.linkStaticLibraryInto(dylib, path.string())) {
            fail("link JIT static dependency '" + path.string() + "': " +
                 error_string(std::move(error)));
        }
        return;
    }
    if (extension == ".so" || extension == ".dylib" || extension == ".dll") {
        dylib.addGenerator(take_expected(
            orc::DynamicLibrarySearchGenerator::Load(
                path.string(), jit.getDataLayout().getGlobalPrefix()),
            "load JIT dependency '" + path.string() + "'"));
    }
}

void add_external_generators(
    orc::LLJIT& jit,
    std::span<std::string const> command)
{
    auto& dylib = jit.getMainJITDylib();
    dylib.addGenerator(take_expected(
        orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit.getDataLayout().getGlobalPrefix()),
        "create current-process ORC symbol generator"));

    auto const search_paths = library_search_paths(command);
    std::set<std::filesystem::path> added;
    for (std::size_t i = 1; i < command.size(); ++i) {
        std::filesystem::path path(command[i]);
        if (std::filesystem::is_regular_file(path)) {
            auto const extension = path.extension().string();
            if ((extension == ".so" || extension == ".dylib" ||
                 extension == ".dll" || extension == ".a" || extension == ".lib") &&
                added.insert(std::filesystem::weakly_canonical(path)).second) {
                add_jit_library(jit, path);
            }
            continue;
        }
        std::string_view arg(command[i]);
        if (!arg.starts_with("-l") || arg.size() <= 2) continue;
        if (auto resolved = resolve_link_library(arg.substr(2), search_paths)) {
            auto canonical = std::filesystem::weakly_canonical(*resolved);
            if (added.insert(canonical).second) add_jit_library(jit, *resolved);
        }
    }
}

iv::AuthoredGraph run_authoring_jit(
    Module const& master,
    orc::ThreadSafeContext context,
    std::span<std::string const> command)
{
    auto authoring = clone_authoring_module(master);
    auto jit = take_expected(orc::LLJITBuilder().create(), "create ORC LLJIT");
    add_external_generators(*jit, command);
    auto tracker = jit->getMainJITDylib().createResourceTracker();
    check_error(jit->addIRModule(
        tracker, orc::ThreadSafeModule(std::move(authoring), context)),
        "add authoring LLVM module to ORC");
    check_error(jit->initialize(jit->getMainJITDylib()), "run authoring global initializers");
    auto address = take_expected(jit->lookup("iv_module_author"), "lookup iv_module_author");
    using AuthorFn = void (*)(iv::GraphBuilder*);
    auto author = address.toPtr<AuthorFn>();
    iv::GraphBuilder builder;
    author(&builder);
    auto authored = std::move(builder).finish();
    check_error(jit->deinitialize(jit->getMainJITDylib()), "run authoring global destructors");
    check_error(tracker->remove(), "release authoring JIT generation");
    return authored;
}

std::vector<std::pair<iv::NodeCodeKey, iv::NodeStateStructure>> bind_state_metadata(
    std::span<IrNodeRecord const> records,
    CompilerMetadata const& metadata)
{
    std::vector<std::pair<iv::NodeCodeKey, iv::NodeStateStructure>> result;
    for (auto const& record : records) {
        auto const normalized = normalize_type_name(record.type_name);
        auto const it = std::find_if(metadata.states.begin(), metadata.states.end(), [&](auto const& item) {
            return normalize_type_name(item.first) == normalized;
        });
        if (it != metadata.states.end()) result.emplace_back(record.key, it->second);
    }
    return result;
}

GlobalVariable* constant_bytes(
    Module& module,
    StringRef name,
    std::span<std::byte const> bytes,
    std::size_t alignment)
{
    auto& context = module.getContext();
    std::vector<std::uint8_t> raw;
    raw.reserve(bytes.size());
    for (auto value : bytes) raw.push_back(std::to_integer<std::uint8_t>(value));
    auto* value = ConstantDataArray::get(context, raw);
    auto* global = new GlobalVariable(
        module, value->getType(), true, GlobalValue::PrivateLinkage,
        value, name);
    global->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    global->setAlignment(Align(std::max<std::size_t>(1, alignment)));
    return global;
}

Function* emit_view_accessor(
    Module& module,
    StringRef name,
    Constant* data,
    std::uint64_t size)
{
    auto& context = module.getContext();
    auto const pointer_bits = module.getDataLayout().getPointerSizeInBits();
    auto* size_type = IntegerType::get(context, pointer_bits);
    auto* view_type = StructType::get(PointerType::getUnqual(context), size_type);
    auto* function_type = FunctionType::get(view_type, false);
    auto* function = Function::Create(
        function_type, GlobalValue::ExternalLinkage, name, module);
    function->setVisibility(GlobalValue::DefaultVisibility);
#if defined(_WIN32)
    function->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
#endif
    auto* block = BasicBlock::Create(context, "entry", function);
    IRBuilder<> builder(block);
    builder.CreateRet(ConstantStruct::get(
        view_type, data, ConstantInt::get(size_type, size)));
    return function;
}

void inject_module_data(
    Module& module,
    iv::SerializedAuthoredGraph const& authored,
    std::span<IrNodeRecord const> node_records)
{
    auto& context = module.getContext();
    auto const pointer_bits = module.getDataLayout().getPointerSizeInBits();
    auto* size_type = IntegerType::get(context, pointer_bits);
    auto* pointer_type = PointerType::getUnqual(context);

    auto graph_bytes = std::as_bytes(std::span{authored.json.data(), authored.json.size()});
    auto* graph = constant_bytes(module, "iv.authored_graph", graph_bytes, 1);
    auto* graph_ptr = ConstantExpr::getPointerCast(graph, pointer_type);
    emit_view_accessor(module, "iv_module_authored_graph", graph_ptr, authored.json.size());

    std::vector<Constant*> config_records;
    auto* config_record_type = StructType::get(pointer_type, size_type, size_type);
    for (std::size_t i = 0; i < authored.node_configs.size(); ++i) {
        auto const& config = authored.node_configs[i];
        auto* bytes = constant_bytes(
            module, "iv.node_config." + std::to_string(i), config.bytes, config.alignment);
        config_records.push_back(ConstantStruct::get(
            config_record_type,
            ConstantExpr::getPointerCast(bytes, pointer_type),
            ConstantInt::get(size_type, config.bytes.size()),
            ConstantInt::get(size_type, config.alignment)));
    }
    auto* config_array_type = ArrayType::get(config_record_type, config_records.size());
    auto* config_array = new GlobalVariable(
        module, config_array_type, true, GlobalValue::PrivateLinkage,
        ConstantArray::get(config_array_type, config_records), "iv.node_configs");
    emit_view_accessor(
        module, "iv_module_node_configs",
        ConstantExpr::getPointerCast(config_array, pointer_type),
        config_records.size() * module.getDataLayout().getTypeAllocSize(config_record_type).getFixedValue());

    if (node_records.empty()) fail("cannot emit empty node type table");
    auto* record_type = node_records.front().initializer->getType();
    std::vector<Constant*> type_records;
    type_records.reserve(node_records.size());
    for (auto const& record : node_records) {
        if (record.initializer->getType() != record_type)
            fail("node compiler records do not share one LLVM type");
        type_records.push_back(record.initializer);
    }
    auto* type_array_type = ArrayType::get(record_type, type_records.size());
    auto* type_array = new GlobalVariable(
        module, type_array_type, true, GlobalValue::PrivateLinkage,
        ConstantArray::get(type_array_type, type_records), "iv.node_types");
    emit_view_accessor(
        module, "iv_module_node_types",
        ConstantExpr::getPointerCast(type_array, pointer_type),
        type_records.size() * module.getDataLayout().getTypeAllocSize(record_type).getFixedValue());
}

std::filesystem::path output_path(std::span<std::string const> command)
{
    for (std::size_t i = 1; i + 1 < command.size(); ++i) {
        if (command[i] == "-o") return command[i + 1];
        if (std::string_view(command[i]).starts_with("-o") && command[i].size() > 2)
            return command[i].substr(2);
    }
    fail("cannot find -o in module link command");
}

void optimize_runtime_module(Module& module, bool optimize)
{
    // The source TUs deliberately stop at O0 LLVM IR. Only after authoring has
    // executed do we spend optimization time on the retained runtime module.
    PassBuilder pass_builder;
    LoopAnalysisManager loops;
    FunctionAnalysisManager functions;
    CGSCCAnalysisManager cgscc;
    ModuleAnalysisManager modules;
    pass_builder.registerModuleAnalyses(modules);
    pass_builder.registerCGSCCAnalyses(cgscc);
    pass_builder.registerFunctionAnalyses(functions);
    pass_builder.registerLoopAnalyses(loops);
    pass_builder.crossRegisterProxies(loops, functions, cgscc, modules);
    auto pipeline = optimize
        ? pass_builder.buildPerModuleDefaultPipeline(OptimizationLevel::O3)
        : pass_builder.buildO0DefaultPipeline(OptimizationLevel::O0);
    pipeline.run(module, modules);
}

void emit_native_object(Module& module, std::filesystem::path const& path, bool optimize)
{
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    auto triple = module.getTargetTriple();
    if (triple.empty()) triple = sys::getDefaultTargetTriple();
    std::string error;
    auto const* target = TargetRegistry::lookupTarget(triple, error);
    if (!target) fail("cannot find target '" + triple + "': " + error);

    TargetOptions options;
    std::unique_ptr<TargetMachine> machine(target->createTargetMachine(
        triple, "generic", "", options, std::nullopt, std::nullopt,
        optimize ? CodeGenOptLevel::Aggressive : CodeGenOptLevel::None));
    if (!machine) fail("cannot create LLVM target machine");
    module.setDataLayout(machine->createDataLayout());
    module.setTargetTriple(triple);

    std::error_code ec;
    raw_fd_ostream output(path.string(), ec, sys::fs::OF_None);
    if (ec) fail("cannot create finalized object '" + path.string() + "': " + ec.message());
    legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, output, nullptr, CodeGenFileType::ObjectFile))
        fail("target does not support object emission");
    passes.run(module);
    output.flush();
}

int run_link_command(
    std::span<std::string const> original,
    std::span<std::filesystem::path const> bitcode_inputs,
    std::filesystem::path const& replacement)
{
    std::set<std::filesystem::path> replaced;
    for (auto const& path : bitcode_inputs) replaced.insert(std::filesystem::weakly_canonical(path));

    std::vector<std::string> args;
    args.reserve(original.size() + 1);
    args.push_back(original.front());
    bool inserted = false;
    for (std::size_t i = 1; i < original.size(); ++i) {
        std::filesystem::path path(original[i]);
        bool replace = false;
        if (!original[i].empty() && original[i][0] != '-' && std::filesystem::exists(path)) {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            replace = !ec && replaced.contains(canonical);
        }
        if (replace) {
            if (!inserted) {
                args.push_back(replacement.string());
                inserted = true;
            }
            continue;
        }
        if (original[i] == "-flto" || std::string_view(original[i]).starts_with("-flto=")) {
            continue;
        }
        args.push_back(original[i]);
    }
    if (!inserted) fail("could not replace LLVM bitcode inputs in linker command");

    SmallVector<StringRef, 64> refs;
    for (auto const& arg : args) refs.push_back(arg);
    auto result = sys::ExecuteAndWait(original.front(), refs);
    if (result < 0) fail("failed to execute final native link");
    return result;
}

int finalize(Options options)
{
    options.link_command = expand_response_files(std::move(options.link_command));

    auto context = orc::ThreadSafeContext(std::make_unique<LLVMContext>());
    auto& llvm_context = *context.getContext();
    auto linked = link_bitcode_inputs(options.link_command, llvm_context);
    auto& master = *linked.module;
    if (master.getDataLayout().isDefault()) {
        // Clang normally writes a data layout into every LTO object. Refuse to
        // invent one because node config size/alignment is ABI-sensitive.
        fail("master LLVM module has no target data layout");
    }

    auto node_records = scan_node_records(master);
    auto metadata = load_metadata(options.metadata_dir);
    auto state_metadata = bind_state_metadata(node_records, metadata);
    auto authored = run_authoring_jit(master, context, options.link_command);
    auto serialized = iv::serialize_authored_graph(authored, state_metadata);
    inject_module_data(master, serialized, node_records);
    if (auto* author = master.getFunction("iv_module_author")) {
        author->setLinkage(GlobalValue::InternalLinkage);
    }
    optimize_runtime_module(master, options.optimize);

    auto output = output_path(options.link_command);
    auto replacement = output;
    replacement += ".iv-finalized.o";
    emit_native_object(master, replacement, options.optimize);
    auto const result = run_link_command(options.link_command, linked.bitcode_inputs, replacement);
    std::error_code ec;
    std::filesystem::remove(replacement, ec);
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return finalize(parse_options(argc, argv));
    } catch (std::exception const& error) {
        llvm::errs() << error.what() << '\n';
        return 1;
    }
}
