#include <intravenous/graph/builder.h>
#include <intravenous/module/builder_session.h>
#include <intravenous/module/configured_graph_wire.h>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
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
#include "llvm/TargetParser/Host.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
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
    std::optional<std::filesystem::path> timings_file;
    bool optimize = true;
    std::vector<std::string> link_command;
};

[[noreturn]] void fail(std::string const& message);

class TimingReport {
    using Clock = std::chrono::steady_clock;

    Clock::time_point const started_at_ = Clock::now();
    std::vector<std::pair<std::string, std::chrono::microseconds>> stages_;

public:
    [[nodiscard]] Clock::time_point start_stage() const noexcept
    {
        return Clock::now();
    }

    void finish_stage(std::string_view name, Clock::time_point started_at)
    {
        stages_.emplace_back(
            name,
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - started_at));
    }

    void write(std::filesystem::path const& path) const
    {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) fail("cannot write timing report '" + path.string() + "'");
        output << "version=1\n";
        for (auto const& [name, duration] : stages_) {
            output << name << "_us=" << duration.count() << '\n';
        }
        output << "total_us="
               << std::chrono::duration_cast<std::chrono::microseconds>(
                      Clock::now() - started_at_)
                      .count()
               << '\n';
        output.flush();
        if (!output) fail("cannot finish timing report '" + path.string() + "'");
    }
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
        if (!command && arg.starts_with("--timings-file=")) {
            auto const path = arg.substr(std::string_view("--timings-file=").size());
            if (path.empty()) fail("empty --timings-file");
            result.timings_file = std::string(path);
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
    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
    Constant* initializer = nullptr;
};

std::uint64_t constant_u64(Value const* value)
{
    auto const* integer = dyn_cast<ConstantInt>(value);
    if (!integer) fail("malformed iv_node_types code key");
    return integer->getZExtValue();
}

std::size_t constant_size(Value const* value, std::string_view field)
{
    auto const result = constant_u64(value);
    if (result > std::numeric_limits<std::size_t>::max()) {
        fail("iv_node_types " + std::string(field) + " does not fit size_t");
    }
    return static_cast<std::size_t>(result);
}

std::vector<IrNodeRecord> scan_node_records(Module& module)
{
    std::vector<IrNodeRecord> result;
    for (auto& global : module.globals()) {
        auto const section = global.getSection();
        if (section != "iv_node_types" && !section.ends_with("__iv_node_types")) continue;
        auto* record = dyn_cast_or_null<ConstantStruct>(global.getInitializer());
        if (!record || record->getNumOperands() != 6) fail("malformed iv_node_types record");
        auto* key = dyn_cast<ConstantStruct>(record->getOperand(0));
        if (!key || key->getNumOperands() != 2) fail("malformed iv_node_types key");
        StringRef name;
        if (!getConstantStringInfo(record->getOperand(2), name, false))
            fail("node compiler record does not contain a constant type name");
        auto const name_size = constant_size(record->getOperand(3), "type name size");
        if (name_size > name.size()) fail("node compiler record type name length is invalid");
        result.push_back({
            .key = {
                .low = constant_u64(key->getOperand(0)),
                .high = constant_u64(key->getOperand(1)),
            },
            .type_name = name.substr(0, name_size).str(),
            .state_size = constant_size(record->getOperand(4), "state size"),
            .state_alignment = constant_size(record->getOperand(5), "state alignment"),
            .initializer = record,
        });
    }
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

std::uint64_t parse_hex_u64(
    StringRef value,
    std::filesystem::path const& metadata_path,
    std::string_view field)
{
    std::uint64_t result = 0;
    auto const [end, error] = std::from_chars(
        value.begin(), value.end(), result, 16);
    if (error != std::errc{} || end != value.end()) {
        fail("invalid " + std::string(field) + " in metadata '" +
             metadata_path.string() + "'");
    }
    return result;
}

std::size_t metadata_size(
    std::int64_t value,
    std::filesystem::path const& metadata_path,
    std::string_view field)
{
    if (value < 0 || static_cast<std::uint64_t>(value)
            > std::numeric_limits<std::size_t>::max()) {
        fail("invalid " + std::string(field) + " in metadata '" +
             metadata_path.string() + "'");
    }
    return static_cast<std::size_t>(value);
}

struct StateMetadata {
    iv::NodeCodeKey key{};
    std::string node_type_usr{};
    std::string state_type_usr{};
    iv::NodeStateStructure structure{};
};

struct ConfigPointerMetadata {
    iv::NodeCodeKey key{};
    std::vector<std::size_t> byte_offsets{};
};

struct RegisteredDefinitionMetadata {
    std::string id;
    std::string kind;
    std::string declaration_usr;
    std::string source_file;
    std::optional<iv::NodeCodeKey> node_code_key;
};

struct CompilerMetadata {
    std::vector<StateMetadata> states;
    std::vector<ConfigPointerMetadata> config_pointers;
    std::vector<RegisteredDefinitionMetadata> registered_definitions;
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
        auto version = object->getInteger("version");
        if (!version || *version != 7) {
            fail("unsupported compiler metadata version in '" + entry.path().string() + "'");
        }
        auto* states = object->getArray("states");
        if (!states) {
            fail("metadata has no state array in '" + entry.path().string() + "'");
        }
        auto* config_pointers = object->getArray("config_pointers");
        if (!config_pointers) {
            fail("metadata has no config-pointer array in '" + entry.path().string() + "'");
        }
        auto* registered_definitions = object->getArray("registered_definitions");
        if (!registered_definitions) {
            fail("metadata has no registered-definition array in '"
                 + entry.path().string() + "'");
        }
        for (auto const& definition_value : *registered_definitions) {
            auto* definition = definition_value.getAsObject();
            if (!definition) {
                fail("registered-definition metadata entry is not an object in '"
                     + entry.path().string() + "'");
            }
            auto id = definition->getString("id");
            auto kind = definition->getString("kind");
            auto declaration_usr = definition->getString("declaration_usr");
            if (!id || id->empty() || !kind || !declaration_usr
                || (kind->str() != "node" && kind->str() != "module")) {
                fail("incomplete registered-definition metadata entry in '"
                     + entry.path().string() + "'");
            }
            RegisteredDefinitionMetadata registered{
                .id = id->str(),
                .kind = kind->str(),
                .declaration_usr = declaration_usr->str(),
                .source_file = {},
                .node_code_key = {},
            };
            if (auto source_file = definition->getString("source_file")) {
                registered.source_file = source_file->str();
            }
            if (registered.kind == "node") {
                auto* key = definition->getObject("node_code_key");
                if (!key) {
                    fail("node registered-definition metadata has no NodeCodeKey in '"
                         + entry.path().string() + "'");
                }
                auto low = key->getString("low");
                auto high = key->getString("high");
                if (!low || !high) {
                    fail("node registered-definition metadata has invalid NodeCodeKey in '"
                         + entry.path().string() + "'");
                }
                registered.node_code_key = {
                    .low = parse_hex_u64(*low, entry.path(), "registered node_code_key.low"),
                    .high = parse_hex_u64(*high, entry.path(), "registered node_code_key.high"),
                };
            }
            auto const duplicate = std::find_if(
                result.registered_definitions.begin(),
                result.registered_definitions.end(),
                [&](RegisteredDefinitionMetadata const& existing) {
                    return existing.id == registered.id
                        && existing.declaration_usr == registered.declaration_usr;
                });
            if (duplicate == result.registered_definitions.end()) {
                result.registered_definitions.push_back(std::move(registered));
            }
        }
        for (auto const& state_value : *states) {
            auto* state = state_value.getAsObject();
            if (!state) fail("state metadata entry is not an object in '" + entry.path().string() + "'");
            auto* key = state->getObject("node_code_key");
            auto node_type_usr = state->getString("node_type_usr");
            auto state_type_usr = state->getString("state_type_usr");
            auto size_bits = state->getInteger("size_bits");
            auto alignment_bits = state->getInteger("alignment_bits");
            auto* fields = state->getArray("fields");
            if (!key || !node_type_usr || !state_type_usr || !size_bits
                || !alignment_bits || !fields) {
                fail("incomplete state metadata entry in '" + entry.path().string() + "'");
            }
            auto low = key->getString("low");
            auto high = key->getString("high");
            if (!low || !high) {
                fail("state metadata has no NodeCodeKey in '" + entry.path().string() + "'");
            }
            StateMetadata metadata{
                .key = {
                    .low = parse_hex_u64(*low, entry.path(), "node_code_key.low"),
                    .high = parse_hex_u64(*high, entry.path(), "node_code_key.high"),
                },
                .node_type_usr = node_type_usr->str(),
                .state_type_usr = state_type_usr->str(),
                .structure = {
                    .size_bits = metadata_size(*size_bits, entry.path(), "state size_bits"),
                    .alignment_bits = metadata_size(*alignment_bits, entry.path(), "state alignment_bits"),
                },
            };
            for (auto const& field_value : *fields) {
                auto* field = field_value.getAsObject();
                if (!field) fail("state field metadata is not an object in '" + entry.path().string() + "'");
                auto name = field->getString("name");
                auto type_usr = field->getString("type_usr");
                auto bit_offset = field->getInteger("bit_offset");
                auto field_size = field->getInteger("size_bits");
                auto field_alignment = field->getInteger("alignment_bits");
                if (!name || !type_usr || !bit_offset || !field_size || !field_alignment) {
                    fail("incomplete state field metadata in '" + entry.path().string() + "'");
                }
                iv::NodeStateFieldStructure item{
                    .name = name->str(),
                    .type_name = type_usr->str(),
                    .bit_offset = metadata_size(*bit_offset, entry.path(), "field bit_offset"),
                    .size_bits = metadata_size(*field_size, entry.path(), "field size_bits"),
                    .alignment_bits = metadata_size(*field_alignment, entry.path(), "field alignment_bits"),
                };
                if (auto width = field->getInteger("bit_width")) {
                    item.bit_width = metadata_size(*width, entry.path(), "field bit_width");
                }
                metadata.structure.fields.push_back(std::move(item));
            }
            auto const duplicate = std::find_if(
                result.states.begin(), result.states.end(),
                [&](StateMetadata const& existing) {
                    return existing.key == metadata.key;
                });
            if (duplicate != result.states.end()) {
                if (duplicate->node_type_usr != metadata.node_type_usr
                    || duplicate->state_type_usr != metadata.state_type_usr
                    || duplicate->structure != metadata.structure) {
                    fail("conflicting state metadata for one NodeCodeKey in '" +
                         entry.path().string() + "'");
                }
                continue;
            }
            result.states.push_back(std::move(metadata));
        }
        for (auto const& field_value : *config_pointers) {
            auto* field = field_value.getAsObject();
            if (!field) fail("config-pointer metadata entry is not an object in '" + entry.path().string() + "'");
            auto* key = field->getObject("node_code_key");
            auto* offsets = field->getArray("byte_offsets");
            if (!key || !offsets) {
                fail("incomplete config-pointer metadata entry in '" + entry.path().string() + "'");
            }
            auto low = key->getString("low");
            auto high = key->getString("high");
            if (!low || !high) {
                fail("config-pointer metadata has no NodeCodeKey in '" + entry.path().string() + "'");
            }
            ConfigPointerMetadata metadata{
                .key = {
                    .low = parse_hex_u64(*low, entry.path(), "node_code_key.low"),
                    .high = parse_hex_u64(*high, entry.path(), "node_code_key.high"),
                },
            };
            for (auto const& offset_value : *offsets) {
                auto offset = offset_value.getAsInteger();
                if (!offset) {
                    fail("config-pointer field offset is not an integer in '" + entry.path().string() + "'");
                }
                metadata.byte_offsets.push_back(
                    metadata_size(*offset, entry.path(), "config-pointer field offset"));
            }
            if (!std::is_sorted(
                    metadata.byte_offsets.begin(), metadata.byte_offsets.end())
                || std::adjacent_find(
                       metadata.byte_offsets.begin(), metadata.byte_offsets.end())
                    != metadata.byte_offsets.end()) {
                fail("config-pointer field offsets must be sorted and unique in '"
                     + entry.path().string() + "'");
            }
            auto const duplicate = std::find_if(
                result.config_pointers.begin(), result.config_pointers.end(),
                [&](ConfigPointerMetadata const& existing) {
                    return existing.key == metadata.key;
                });
            if (duplicate != result.config_pointers.end()) {
                if (duplicate->byte_offsets != metadata.byte_offsets) {
                    fail("conflicting config-pointer metadata for one NodeCodeKey in '" +
                         entry.path().string() + "'");
                }
                continue;
            }
            result.config_pointers.push_back(std::move(metadata));
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

struct BuilderModuleClone {
    struct RetainedGlobal {
        GlobalVariable const* master = nullptr;
        std::size_t size = 0;
    };

    std::unique_ptr<Module> module;
    std::vector<RetainedGlobal> retained_globals;
};

void add_retained_global_address_table(
    Module& module,
    std::span<GlobalVariable* const> globals)
{
    auto& context = module.getContext();
    auto* pointer_type = PointerType::getUnqual(context);
    std::vector<Constant*> entries;
    entries.reserve(globals.size());
    for (auto* global : globals) {
        entries.push_back(ConstantExpr::getPointerCast(global, pointer_type));
    }
    auto* table_type = ArrayType::get(pointer_type, entries.size());
    auto* table = new GlobalVariable(
        module,
        table_type,
        true,
        GlobalValue::PrivateLinkage,
        ConstantArray::get(table_type, entries),
        "iv.retained_global_addresses");
    table->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

    auto* function_type = FunctionType::get(pointer_type, false);
    auto* function = Function::Create(
        function_type,
        GlobalValue::ExternalLinkage,
        "iv_get_retained_global_addresses",
        module);
    auto* block = BasicBlock::Create(context, "entry", function);
    IRBuilder<> builder(block);
    builder.CreateRet(ConstantExpr::getPointerCast(table, pointer_type));
}

BuilderModuleClone clone_builder_module(Module const& master)
{
    SmallPtrSet<GlobalValue const*, 32> reachable;
    static constexpr std::array<StringRef, 1> configuration_entry_points{
        "iv_package_registrations",
    };
    for (auto const name : configuration_entry_points) {
        auto const* entry_point = master.getFunction(name);
        if (!entry_point || entry_point->isDeclaration()) {
            fail("master LLVM module does not define graph-configuration entry point '"
                 + name.str() + "'");
        }
        mark_reachable(entry_point, reachable);
    }
    if (auto const* ctors = master.getGlobalVariable("llvm.global_ctors")) mark_reachable(ctors, reachable);
    if (auto const* dtors = master.getGlobalVariable("llvm.global_dtors")) mark_reachable(dtors, reachable);

    ValueToValueMapTy map;
    auto cloned_module = CloneModule(master, map, [&](GlobalValue const* global) {
        if (global->isDeclaration()) return false;
        return reachable.contains(global);
    });

    BuilderModuleClone result{.module = std::move(cloned_module), .retained_globals = {}};
    std::vector<GlobalVariable*> cloned_globals;
    for (auto const& global : master.globals()) {
        if (!global.isConstant() || global.isDeclaration() || !global.hasInitializer()) {
            continue;
        }
        auto* cloned = dyn_cast_or_null<GlobalVariable>(map.lookup(&global));
        // CloneModule records mappings for filtered-out globals too, but those
        // values are declarations in the configuration clone. Referencing one from
        // the address table would turn an otherwise irrelevant module symbol
        // into an ORC lookup dependency.
        if (!cloned || !cloned->hasInitializer()) continue;
        auto const size = master.getDataLayout().getTypeAllocSize(global.getValueType());
        if (size.isScalable() || size.getFixedValue() == 0) continue;
        result.retained_globals.push_back({
            .master = &global,
            .size = size.getFixedValue(),
        });
        cloned_globals.push_back(cloned);
    }
    add_retained_global_address_table(*result.module, cloned_globals);
    return result;
}

void mark_runtime_module_roots(
    Module const& module,
    SmallPtrSetImpl<GlobalValue const*>& reachable)
{
    // Keep the source's graph-configuration callbacks and immutable metadata
    // alive in the finalized binary. Loaded graphs pin the binary while any of
    // these addresses can still be referenced.
    static constexpr std::array<StringRef, 6> runtime_entry_points{
        "iv_module_abi_version",
        "iv_module_node_types",
        "iv_package_node_types",
        "iv_package_registrations",
        "iv_package_node_config_pointer_fields",
        "iv_package_retained_globals",
    };

    for (auto const name : runtime_entry_points) {
        auto const* entry_point = module.getFunction(name);
        if (!entry_point || entry_point->isDeclaration()) {
            fail("finalized LLVM module does not define runtime entry point '" +
                 name.str() + "'");
        }
        mark_reachable(entry_point, reachable);
    }
    if (auto const* ctors = module.getGlobalVariable("llvm.global_ctors")) {
        mark_reachable(ctors, reachable);
    }
    if (auto const* dtors = module.getGlobalVariable("llvm.global_dtors")) {
        mark_reachable(dtors, reachable);
    }
}

void preserve_source_configuration_ir(Module& module)
{
    SmallPtrSet<GlobalValue const*, 32> runtime_reachable;
    mark_runtime_module_roots(module, runtime_reachable);
    SmallVector<GlobalValue*, 32> retained_entries;
    retained_entries.reserve(runtime_reachable.size());
    for (auto const* value : runtime_reachable) {
        if (value->isDeclaration()
            || value->getName() == "llvm.used"
            || value->getName() == "llvm.compiler.used") {
            continue;
        }
        retained_entries.push_back(const_cast<GlobalValue*>(value));
    }
    appendToUsed(module, retained_entries);
    legacy::PassManager pipeline;
    pipeline.add(createGlobalDCEPass());
    pipeline.run(module);

    static constexpr std::array<StringRef, 3> configuration_entry_points{
        "iv_package_registrations",
        "iv_package_node_config_pointer_fields",
        "iv_package_retained_globals",
    };
    for (auto const name : configuration_entry_points) {
        if (!module.getFunction(name) || module.getFunction(name)->isDeclaration()) {
            fail("source configuration entry point was removed: '" + name.str() + "'");
        }
    }
}

struct BuilderJitNodeType {
    std::string id;
    iv::NodeCodeKey code_key{};
    iv::ConfiguredGraph graph{};
};

struct BuilderJitResult {
    std::vector<BuilderJitNodeType> node_types;
    std::vector<BuilderModuleClone::RetainedGlobal> retained_globals;
};

void configure_node_type_ports(iv::GraphBuilder& builder, iv::NodeRef node)
{
    for (std::size_t input = 0; input < node.sample_input_count(); ++input) {
        auto const config = builder.sample_input_config(node.node_bundle_handle(), input);
        auto const name = config.name.empty()
            ? std::string("input") + std::to_string(input)
            : config.name;
        node.connect_input(input, builder.input_named(
            name,
            config.channel_layout,
            config.default_value,
            config.min,
            config.max));
    }
    for (std::size_t input = 0; input < node.event_input_count(); ++input) {
        auto const config = builder.event_input_config(node.node_bundle_handle(), input);
        auto const name = config.name.empty()
            ? std::string("eventInput") + std::to_string(input)
            : config.name;
        node.connect_event_input(
            input, builder.event_input_named(name, config.type));
    }

    std::vector<iv::SampleOutputRequest> sample_outputs;
    std::vector<std::string> sample_output_names;
    sample_outputs.reserve(node.sample_output_count());
    sample_output_names.reserve(node.sample_output_count());
    for (std::size_t output = 0; output < node.sample_output_count(); ++output) {
        auto port = node[output];
        sample_output_names.push_back("output" + std::to_string(output));
        auto const& name = sample_output_names.back();
        sample_outputs.push_back({
            .ref = port,
            .name = name,
            .channel_layout = {
                .channel_type = port.channel_type,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .family_name = name,
            .family_channel_type = port.channel_type,
        });
    }
    if (!sample_outputs.empty()) {
        builder.outputs(std::span<iv::SampleOutputRequest const>(sample_outputs));
    }

    std::vector<iv::EventOutputRequest> event_outputs;
    std::vector<std::string> event_output_names;
    event_outputs.reserve(node.event_output_count());
    event_output_names.reserve(node.event_output_count());
    for (std::size_t output = 0; output < node.event_output_count(); ++output) {
        event_output_names.push_back("eventOutput" + std::to_string(output));
        event_outputs.push_back({
            .ref = node.event_port(output),
            .name = event_output_names.back(),
        });
    }
    if (!event_outputs.empty()) {
        builder.event_outputs(std::span<iv::EventOutputRequest const>(event_outputs));
    }
}

BuilderJitResult run_builder_jit(
    Module const& master,
    orc::ThreadSafeContext context,
    std::span<std::string const> command,
    CompilerMetadata const& metadata,
    TimingReport& timings)
{
    auto stage_started_at = timings.start_stage();
    auto builder_clone = clone_builder_module(master);
    auto builder_module = std::move(builder_clone.module);
    timings.finish_stage("configuration_module_clone", stage_started_at);

    stage_started_at = timings.start_stage();
    initialize_native_target();
    auto jit_target = take_expected(
        orc::JITTargetMachineBuilder::detectHost(),
        "detect ORC JIT target");
    jit_target.setCodeGenOptLevel(CodeGenOptLevel::None);
    auto jit = take_expected(
        orc::LLJITBuilder()
            .setJITTargetMachineBuilder(std::move(jit_target))
            .create(),
        "create ORC LLJIT");
    timings.finish_stage("jit_create", stage_started_at);

    stage_started_at = timings.start_stage();
    add_external_generators(*jit, command);
    auto tracker = jit->getMainJITDylib().createResourceTracker();
    check_error(jit->addIRModule(
        tracker, orc::ThreadSafeModule(std::move(builder_module), context)),
        "add configuration LLVM module to ORC");
    check_error(
        jit->initialize(jit->getMainJITDylib()),
        "run IV package global initializers");
    auto registrations_address = take_expected(
        jit->lookup("iv_package_registrations"),
        "lookup iv_package_registrations");
    auto global_addresses = take_expected(
        jit->lookup("iv_get_retained_global_addresses"),
        "lookup retained LLVM global address table");
    timings.finish_stage("jit_materialize", stage_started_at);

    using RegistrationsFn = iv::ModuleDataView (*)();
    auto const registration_view =
        registrations_address.toPtr<RegistrationsFn>()();
    if (!registration_view.data && registration_view.size != 0) {
        fail("IV package registration table has null data");
    }
    if (registration_view.size % sizeof(iv::details::PackageRegistrationView) != 0) {
        fail("IV package registration table has invalid size");
    }
    auto const registrations = std::span(
        static_cast<iv::details::PackageRegistrationView const*>(registration_view.data),
        registration_view.size / sizeof(iv::details::PackageRegistrationView));

    std::string source_root;
    if (!registrations.empty()) {
        auto const& first = registrations.front();
        if (!first.source_root || first.source_root_size == 0) {
            fail("IV package registration has no source root");
        }
        source_root.assign(first.source_root, first.source_root_size);
        for (auto const& registration : registrations) {
            if (!registration.source_root
                || std::string_view(registration.source_root, registration.source_root_size)
                    != source_root) {
                fail("one IV package emitted registrations for multiple source roots");
            }
        }
    }

    stage_started_at = timings.start_stage();
    std::vector<iv::NodeConfigPointerFieldData> pointer_fields;
    for (auto const& entry : metadata.config_pointers) {
        for (auto const offset : entry.byte_offsets) {
            pointer_fields.push_back({
                .code_key = entry.key,
                .byte_offset = offset,
            });
        }
    }
    using GlobalAddressTableFn = void const* (*)();
    auto const table = global_addresses.toPtr<GlobalAddressTableFn>()();
    auto const* addresses = static_cast<void const* const*>(table);
    std::vector<iv::RetainedGlobalData> retained_globals;
    retained_globals.reserve(builder_clone.retained_globals.size());
    for (std::size_t ordinal = 0; ordinal < builder_clone.retained_globals.size(); ++ordinal) {
        retained_globals.push_back({
            .address = addresses[ordinal],
            .size = builder_clone.retained_globals[ordinal].size,
            .ordinal = ordinal,
        });
    }
    timings.finish_stage("builder_session_setup", stage_started_at);

    stage_started_at = timings.start_stage();
    std::vector<BuilderJitNodeType> node_types;
    std::set<std::string> registered_ids;
    std::set<std::string> runtime_node_ids;
    std::set<std::string> runtime_module_ids;
    auto const has_registration_metadata = !metadata.registered_definitions.empty();

    for (auto const& registration : registrations) {
        if (!registration.id || registration.id_size == 0) {
            fail("IV package registration has an empty ID");
        }
        auto name = std::string(registration.id, registration.id_size);
        if (!registered_ids.insert(name).second) {
            fail("duplicate registered IV definition ID within one IV package: '"
                 + name + "'");
        }

        if (registration.kind == iv::details::PackageRegistrationKind::module) {
            if (!registration.module_build) {
                fail("registered IV module '" + name + "' has no build function");
            }
            runtime_module_ids.insert(name);
            if (has_registration_metadata) {
                auto const metadata_module = std::find_if(
                    metadata.registered_definitions.begin(),
                    metadata.registered_definitions.end(),
                    [&](RegisteredDefinitionMetadata const& definition) {
                        return definition.kind == "module" && definition.id == name;
                    });
                if (metadata_module == metadata.registered_definitions.end()) {
                    fail("registered IV module '" + name
                         + "' has no matching compiler registration metadata");
                }
            }
            continue;
        }

        if (registration.kind != iv::details::PackageRegistrationKind::node
            || !registration.node_build || !registration.node_compiler_record) {
            fail("registered IV node '" + name + "' is incomplete");
        }
        runtime_node_ids.insert(name);
        auto const* compiler_record = static_cast<iv::details::NodeCompilerRecord const*>(
            registration.node_compiler_record);
        auto const key = compiler_record->code_key;
        if (has_registration_metadata) {
            auto const metadata_node = std::find_if(
                metadata.registered_definitions.begin(),
                metadata.registered_definitions.end(),
                [&](RegisteredDefinitionMetadata const& definition) {
                    return definition.kind == "node" && definition.id == name
                        && definition.node_code_key == key;
                });
            if (metadata_node == metadata.registered_definitions.end()) {
                fail("registered IV node '" + name
                     + "' has no matching compiler metadata/NodeCodeKey");
            }
        }

        auto node_session = std::unique_ptr<
            iv::details::BuilderSession,
            decltype(&iv::details::iv_builder_session_destroy)>(
                iv::details::iv_builder_session_create(),
                iv::details::iv_builder_session_destroy);
        if (!node_session) fail("create IV package node-type builder session");
        iv::details::BuilderPackageView const source{
            .source_root = source_root,
            .registrations = registrations,
            .config_pointer_fields = pointer_fields,
            .retained_globals = retained_globals,
        };
        iv::details::set_builder_packages(node_session.get(), std::span(&source, 1));
        iv::details::select_builder_package(node_session.get(), 0);
        iv::GraphBuilder builder(node_session.get());
        auto node = registration.node_build(builder);
        configure_node_type_ports(builder, node);
        node_types.push_back({
            .id = std::move(name),
            .code_key = key,
            .graph = iv::details::take_built_graph(node_session.get()),
        });
    }

    if (has_registration_metadata) {
        for (auto const& definition : metadata.registered_definitions) {
            auto const& runtime_ids = definition.kind == "node"
                ? runtime_node_ids
                : runtime_module_ids;
            if (!runtime_ids.contains(definition.id)) {
                fail("compiler metadata registered " + definition.kind + " '"
                     + definition.id
                     + "' but the source registration table did not publish it");
            }
        }
    }
    timings.finish_stage("package_registration_validation", stage_started_at);

    stage_started_at = timings.start_stage();
    check_error(
        jit->deinitialize(jit->getMainJITDylib()),
        "run IV package global destructors");
    check_error(tracker->remove(), "release IV package JIT code");
    timings.finish_stage("jit_release", stage_started_at);
    return {
        .node_types = std::move(node_types),
        .retained_globals = std::move(builder_clone.retained_globals),
    };
}

std::vector<std::pair<iv::NodeCodeKey, iv::NodeStateStructure>> bind_state_metadata(
    std::span<IrNodeRecord const> records,
    CompilerMetadata const& metadata)
{
    std::vector<std::pair<iv::NodeCodeKey, iv::NodeStateStructure>> result;
    for (auto const& record : records) {
        auto const state = std::find_if(metadata.states.begin(), metadata.states.end(), [&](auto const& item) {
            return item.key == record.key;
        });
        if (record.state_size == 0) {
            if (state != metadata.states.end()) {
                fail("state metadata names a node without Node::State: '" +
                     record.type_name + "'");
            }
            continue;
        }
        if (record.state_size > std::numeric_limits<std::size_t>::max() / 8
            || record.state_alignment > std::numeric_limits<std::size_t>::max() / 8) {
            fail("Node::State ABI size overflows for '" + record.type_name + "'");
        }
        if (state == metadata.states.end()) {
            fail("missing NodeCodeKey-bound state metadata for '" +
                 record.type_name + "'");
        }
        if (state->structure.size_bits != record.state_size * 8
            || state->structure.alignment_bits != record.state_alignment * 8) {
            fail("Node::State layout metadata does not match compiler ABI for '" +
                 record.type_name + "'");
        }
        result.emplace_back(record.key, state->structure);
    }
    return result;
}

void require_node_config_metadata(
    std::span<IrNodeRecord const> records,
    CompilerMetadata const& metadata)
{
    for (auto const& record : records) {
        auto const layout = std::find_if(
            metadata.config_pointers.begin(), metadata.config_pointers.end(),
            [&](ConfigPointerMetadata const& item) {
                return item.key == record.key;
            });
        if (layout == metadata.config_pointers.end()) {
            fail("missing NodeCodeKey-bound node configuration metadata for '"
                 + record.type_name + "'");
        }
    }
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


void inject_package_registration_table(Module& module)
{
    std::vector<GlobalVariable*> registrations;
    for (auto& global : module.globals()) {
        if (global.getSection() == iv::details::package_registration_section
            && global.isConstant() && global.hasInitializer()) {
            registrations.push_back(&global);
        }
    }
    std::ranges::sort(registrations, {}, [](GlobalVariable const* registration) {
        return registration->getName();
    });

    if (registrations.empty()) {
        emit_view_accessor(
            module,
            "iv_package_registrations",
            ConstantPointerNull::get(PointerType::getUnqual(module.getContext())),
            0);
        return;
    }

    auto* record_type = registrations.front()->getValueType();
    for (auto const* registration : registrations) {
        if (registration->getValueType() != record_type) {
            fail("IV package registration records have inconsistent LLVM types");
        }
    }
    auto const record_size = module.getDataLayout().getTypeAllocSize(record_type);
    if (record_size.isScalable()
        || record_size.getFixedValue() != sizeof(iv::details::PackageRegistrationView)) {
        fail("IV package registration record ABI does not match PackageRegistrationView");
    }

    std::vector<Constant*> values;
    values.reserve(registrations.size());
    for (auto const* registration : registrations) {
        values.push_back(registration->getInitializer());
    }
    auto* array_type = ArrayType::get(record_type, values.size());
    auto* table = new GlobalVariable(
        module,
        array_type,
        true,
        GlobalValue::PrivateLinkage,
        ConstantArray::get(array_type, values),
        "iv.package_registrations");
    emit_view_accessor(
        module,
        "iv_package_registrations",
        ConstantExpr::getPointerCast(table, PointerType::getUnqual(module.getContext())),
        values.size() * record_size.getFixedValue());
}

void inject_source_configuration_metadata(
    Module& module,
    CompilerMetadata const& metadata,
    std::span<BuilderModuleClone::RetainedGlobal const> retained_globals)
{
    auto& context = module.getContext();
    auto const pointer_bits = module.getDataLayout().getPointerSizeInBits();
    auto* size_type = IntegerType::get(context, pointer_bits);
    auto* pointer_type = PointerType::getUnqual(context);
    auto* i64 = Type::getInt64Ty(context);
    auto* code_key_type = StructType::get(i64, i64);
    auto* pointer_field_type = StructType::get(code_key_type, size_type);

    std::vector<Constant*> pointer_fields;
    for (auto const& entry : metadata.config_pointers) {
        for (auto const offset : entry.byte_offsets) {
            pointer_fields.push_back(ConstantStruct::get(
                pointer_field_type,
                ConstantStruct::get(
                    code_key_type,
                    ConstantInt::get(i64, entry.key.low),
                    ConstantInt::get(i64, entry.key.high)),
                ConstantInt::get(size_type, offset)));
        }
    }
    if (pointer_fields.empty()) {
        emit_view_accessor(
            module,
            "iv_package_node_config_pointer_fields",
            ConstantPointerNull::get(pointer_type),
            0);
    } else {
        auto* array_type = ArrayType::get(pointer_field_type, pointer_fields.size());
        auto* table = new GlobalVariable(
            module,
            array_type,
            true,
            GlobalValue::PrivateLinkage,
            ConstantArray::get(array_type, pointer_fields),
            "iv.source_node_config_pointer_fields");
        emit_view_accessor(
            module,
            "iv_package_node_config_pointer_fields",
            ConstantExpr::getPointerCast(table, pointer_type),
            pointer_fields.size()
                * module.getDataLayout().getTypeAllocSize(pointer_field_type).getFixedValue());
    }

    auto* retained_global_type = StructType::get(pointer_type, size_type, size_type);
    std::vector<Constant*> globals;
    globals.reserve(retained_globals.size());
    for (std::size_t ordinal = 0; ordinal < retained_globals.size(); ++ordinal) {
        auto const& global = retained_globals[ordinal];
        globals.push_back(ConstantStruct::get(
            retained_global_type,
            ConstantExpr::getPointerCast(
                const_cast<GlobalVariable*>(global.master), pointer_type),
            ConstantInt::get(size_type, global.size),
            ConstantInt::get(size_type, ordinal)));
    }
    if (globals.empty()) {
        emit_view_accessor(
            module,
            "iv_package_retained_globals",
            ConstantPointerNull::get(pointer_type),
            0);
    } else {
        auto* array_type = ArrayType::get(retained_global_type, globals.size());
        auto* table = new GlobalVariable(
            module,
            array_type,
            true,
            GlobalValue::PrivateLinkage,
            ConstantArray::get(array_type, globals),
            "iv.source_retained_globals");
        emit_view_accessor(
            module,
            "iv_package_retained_globals",
            ConstantExpr::getPointerCast(table, pointer_type),
            globals.size()
                * module.getDataLayout().getTypeAllocSize(retained_global_type).getFixedValue());
    }
}

Constant* byte_array_constant(
    LLVMContext& context,
    std::span<std::byte const> bytes)
{
    std::vector<std::uint8_t> raw;
    raw.reserve(bytes.size());
    for (auto value : bytes) raw.push_back(std::to_integer<std::uint8_t>(value));
    return ConstantDataArray::get(context, raw);
}

GlobalVariable* constant_node_config(
    Module& module,
    StringRef name,
    iv::ConfiguredNodeConfigBytes const& config,
    std::span<BuilderModuleClone::RetainedGlobal const> retained_globals)
{
    auto& context = module.getContext();
    auto* pointer_type = PointerType::getUnqual(context);
    auto const pointer_size = module.getDataLayout().getPointerSize();
    std::vector<Type*> field_types;
    std::vector<Constant*> field_values;
    std::size_t cursor = 0;

    auto append_bytes = [&](std::size_t begin, std::size_t end) {
        if (begin == end) return;
        auto const bytes = std::span<std::byte const>(
            config.bytes.data() + begin, end - begin);
        auto* value = byte_array_constant(context, bytes);
        field_types.push_back(value->getType());
        field_values.push_back(value);
    };

    for (auto const& relocation : config.relocations) {
        if (relocation.byte_offset < cursor
            || relocation.byte_offset > config.bytes.size()
            || config.bytes.size() - relocation.byte_offset < pointer_size) {
            fail("node configuration pointer relocations overlap or are out of bounds");
        }
        append_bytes(cursor, relocation.byte_offset);

        Constant* target = ConstantPointerNull::get(pointer_type);
        if (relocation.retained_global_ordinal) {
            auto const ordinal = *relocation.retained_global_ordinal;
            if (ordinal >= retained_globals.size()
                || relocation.addend >= retained_globals[ordinal].size) {
                fail("node configuration references an unknown retained LLVM global");
            }
            auto* global = const_cast<GlobalVariable*>(retained_globals[ordinal].master);
            target = ConstantExpr::getPointerCast(global, pointer_type);
            if (relocation.addend != 0) {
                std::array<Constant*, 1> index{
                    ConstantInt::get(
                        IntegerType::get(context, module.getDataLayout().getPointerSizeInBits()),
                        relocation.addend),
                };
                target = ConstantExpr::getInBoundsGetElementPtr(
                    Type::getInt8Ty(context), target, index);
            }
        }
        field_types.push_back(pointer_type);
        field_values.push_back(target);
        cursor = relocation.byte_offset + pointer_size;
    }
    append_bytes(cursor, config.bytes.size());

    auto* storage_type = StructType::get(context, field_types, true);
    auto* global = new GlobalVariable(
        module,
        storage_type,
        true,
        GlobalValue::PrivateLinkage,
        ConstantStruct::get(storage_type, field_values),
        name);
    global->setAlignment(Align(std::max<std::size_t>(1, config.alignment)));
    return global;
}

Function* emit_indexed_view_accessor(
    Module& module,
    StringRef name,
    GlobalVariable* views,
    std::size_t count)
{
    auto& context = module.getContext();
    auto const pointer_bits = module.getDataLayout().getPointerSizeInBits();
    auto* size_type = IntegerType::get(context, pointer_bits);
    auto* pointer_type = PointerType::getUnqual(context);
    auto* view_type = StructType::get(pointer_type, size_type);
    auto* function_type = FunctionType::get(view_type, {size_type}, false);
    auto* function = Function::Create(
        function_type, GlobalValue::ExternalLinkage, name, module);
    function->setVisibility(GlobalValue::DefaultVisibility);
#if defined(_WIN32)
    function->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
#endif
    auto* block = BasicBlock::Create(context, "entry", function);
    IRBuilder<> builder(block);
    if (count == 0) {
        builder.CreateRet(ConstantAggregateZero::get(view_type));
        return function;
    }
    auto* index = function->getArg(0);
    auto* valid = builder.CreateICmpULT(
        index, ConstantInt::get(size_type, count));
    auto* valid_block = BasicBlock::Create(context, "valid", function);
    auto* invalid_block = BasicBlock::Create(context, "invalid", function);
    builder.CreateCondBr(valid, valid_block, invalid_block);

    builder.SetInsertPoint(valid_block);
    auto* element = builder.CreateInBoundsGEP(
        views->getValueType(), views, {ConstantInt::get(size_type, 0), index});
    auto* value = builder.CreateLoad(view_type, element);
    builder.CreateRet(value);

    builder.SetInsertPoint(invalid_block);
    builder.CreateRet(ConstantAggregateZero::get(view_type));
    return function;
}

struct SerializedSourceNodeType {
    std::string id;
    iv::NodeCodeKey code_key{};
    iv::SerializedConfiguredGraph configured;
};

void inject_source_data(
    Module& module,
    std::span<SerializedSourceNodeType const> source_node_types,
    std::span<IrNodeRecord const> node_records,
    std::span<BuilderModuleClone::RetainedGlobal const> retained_globals)
{
    auto& context = module.getContext();
    auto const pointer_bits = module.getDataLayout().getPointerSizeInBits();
    auto* size_type = IntegerType::get(context, pointer_bits);
    auto* pointer_type = PointerType::getUnqual(context);
    auto* view_type = StructType::get(pointer_type, size_type);
    auto* config_record_type = StructType::get(pointer_type, size_type, size_type);

    auto* i64 = Type::getInt64Ty(context);
    auto* code_key_type = StructType::get(i64, i64);
    auto* source_node_type_type = StructType::get(
        view_type, code_key_type, view_type, view_type);
    if (source_node_types.empty()) {
        emit_view_accessor(
            module,
            "iv_package_node_types",
            ConstantPointerNull::get(pointer_type),
            0);
    } else {
        std::vector<Constant*> source_node_type_records;
        source_node_type_records.reserve(source_node_types.size());
        for (std::size_t index = 0; index < source_node_types.size(); ++index) {
            auto const& source_node_type = source_node_types[index];
            auto const id_bytes = std::as_bytes(std::span(
                source_node_type.id.data(), source_node_type.id.size()));
            auto* id = constant_bytes(
                module, "iv.source_node_type_id." + std::to_string(index), id_bytes, 1);
            auto* id_view = ConstantStruct::get(
                view_type,
                ConstantExpr::getPointerCast(id, pointer_type),
                ConstantInt::get(size_type, source_node_type.id.size()));
            auto* code_key = ConstantStruct::get(
                code_key_type,
                ConstantInt::get(i64, source_node_type.code_key.low),
                ConstantInt::get(i64, source_node_type.code_key.high));
            auto const graph_bytes = std::span<std::byte const>(
                source_node_type.configured.bytes.data(),
                source_node_type.configured.bytes.size());
            auto* graph = constant_bytes(
                module,
                "iv.source_node_type_configured_graph." + std::to_string(index),
                graph_bytes,
                1);
            auto* graph_view = ConstantStruct::get(
                view_type,
                ConstantExpr::getPointerCast(graph, pointer_type),
                ConstantInt::get(
                    size_type, source_node_type.configured.bytes.size()));
            std::vector<Constant*> config_records;
            config_records.reserve(source_node_type.configured.node_configs.size());
            for (std::size_t config_index = 0;
                 config_index < source_node_type.configured.node_configs.size();
                 ++config_index) {
                auto const& config = source_node_type.configured.node_configs[config_index];
                auto* bytes = constant_node_config(
                    module,
                    "iv.source_node_type_config." + std::to_string(index)
                        + "." + std::to_string(config_index),
                    config,
                    retained_globals);
                config_records.push_back(ConstantStruct::get(
                    config_record_type,
                    ConstantExpr::getPointerCast(bytes, pointer_type),
                    ConstantInt::get(size_type, config.bytes.size()),
                    ConstantInt::get(size_type, config.alignment)));
            }
            auto* config_array_type = ArrayType::get(
                config_record_type, config_records.size());
            auto* config_array = new GlobalVariable(
                module,
                config_array_type,
                true,
                GlobalValue::PrivateLinkage,
                ConstantArray::get(config_array_type, config_records),
                "iv.source_node_type_configs." + std::to_string(index));
            auto* config_view = ConstantStruct::get(
                view_type,
                ConstantExpr::getPointerCast(config_array, pointer_type),
                ConstantInt::get(
                    size_type,
                    config_records.size()
                        * module.getDataLayout()
                              .getTypeAllocSize(config_record_type)
                              .getFixedValue()));
            source_node_type_records.push_back(ConstantStruct::get(
                source_node_type_type, id_view, code_key, graph_view, config_view));
        }
        auto* source_node_type_array_type = ArrayType::get(
            source_node_type_type, source_node_type_records.size());
        auto* source_node_type_array = new GlobalVariable(
            module,
            source_node_type_array_type,
            true,
            GlobalValue::PrivateLinkage,
            ConstantArray::get(source_node_type_array_type, source_node_type_records),
            "iv.source_node_types");
        emit_view_accessor(
            module,
            "iv_package_node_types",
            ConstantExpr::getPointerCast(source_node_type_array, pointer_type),
            source_node_type_records.size()
                * module.getDataLayout().getTypeAllocSize(source_node_type_type)
                      .getFixedValue());
    }

    if (node_records.empty()) {
        emit_view_accessor(
            module,
            "iv_module_node_types",
            ConstantPointerNull::get(pointer_type),
            0);
        return;
    }
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
    // The source TUs deliberately stop at O0 LLVM IR. Only after graph building has
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
    initialize_native_target();

    Triple triple = module.getTargetTriple();
    if (triple.str().empty()) triple = Triple(sys::getDefaultTargetTriple());
    std::string error;
    auto const* target = TargetRegistry::lookupTarget(triple, error);
    if (!target) fail("cannot find target '" + triple.str() + "': " + error);

    TargetOptions options;
    std::unique_ptr<TargetMachine> machine(target->createTargetMachine(
        triple, "generic", "", options, Reloc::PIC_, std::nullopt,
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
    TimingReport timings;

    auto stage_started_at = timings.start_stage();
    options.link_command = expand_response_files(std::move(options.link_command));
    auto context = orc::ThreadSafeContext(std::make_unique<LLVMContext>());
    auto linked = context.withContextDo([&](LLVMContext* llvm_context) {
        return link_bitcode_inputs(options.link_command, *llvm_context);
    });
    timings.finish_stage("bitcode_parse_link", stage_started_at);

    auto& master = *linked.module;
    if (master.getDataLayout().isDefault()) {
        // Clang normally writes a data layout into every LTO object. Refuse to
        // invent one because node config size/alignment is ABI-sensitive.
        fail("master LLVM module has no target data layout");
    }

    stage_started_at = timings.start_stage();
    auto node_records = scan_node_records(master);
    timings.finish_stage("node_record_scan", stage_started_at);

    stage_started_at = timings.start_stage();
    auto metadata = load_metadata(options.metadata_dir);
    timings.finish_stage("metadata_load", stage_started_at);

    stage_started_at = timings.start_stage();
    auto state_metadata = bind_state_metadata(node_records, metadata);
    require_node_config_metadata(node_records, metadata);
    timings.finish_stage("metadata_bind", stage_started_at);

    stage_started_at = timings.start_stage();
    inject_package_registration_table(master);
    timings.finish_stage("package_registration_table", stage_started_at);

    auto configured = run_builder_jit(
        master, context, options.link_command, metadata, timings);

    stage_started_at = timings.start_stage();
    std::vector<SerializedSourceNodeType> serialized_node_types;
    serialized_node_types.reserve(configured.node_types.size());
    for (auto const& node_type : configured.node_types) {
        serialized_node_types.push_back({
            .id = node_type.id,
            .code_key = node_type.code_key,
            .configured = iv::serialize_configured_graph(
                node_type.graph, state_metadata),
        });
    }
    timings.finish_stage("source_graphs_serialize", stage_started_at);

    stage_started_at = timings.start_stage();
    inject_source_data(
        master, serialized_node_types, node_records,
        configured.retained_globals);
    inject_source_configuration_metadata(
        master, metadata, configured.retained_globals);
    timings.finish_stage("source_data_inject", stage_started_at);

    stage_started_at = timings.start_stage();
    preserve_source_configuration_ir(master);
    timings.finish_stage("source_configuration_ir_preserve", stage_started_at);

    stage_started_at = timings.start_stage();
    optimize_runtime_module(master, options.optimize);
    timings.finish_stage("runtime_optimize", stage_started_at);

    auto output = output_path(options.link_command);
    auto replacement = output;
    replacement += ".iv-finalized.o";

    stage_started_at = timings.start_stage();
    emit_native_object(master, replacement, options.optimize);
    timings.finish_stage("native_object_emit", stage_started_at);

    stage_started_at = timings.start_stage();
    auto const result = run_link_command(options.link_command, linked.bitcode_inputs, replacement);
    timings.finish_stage("native_link", stage_started_at);

    std::error_code ec;
    std::filesystem::remove(replacement, ec);
    if (options.timings_file) timings.write(*options.timings_file);
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
