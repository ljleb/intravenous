#include <intravenous/module/abi.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/node/node_state_structure.h>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
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
    std::filesystem::path output;
    std::vector<std::string> bitcode_inputs;
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
    throw std::runtime_error("iv-package-finalize: " + message);
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
    bool inputs = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (!inputs && arg == "--") {
            inputs = true;
            continue;
        }
        if (!inputs && arg.starts_with("--metadata-dir=")) {
            result.metadata_dir = std::string(arg.substr(std::string_view("--metadata-dir=").size()));
            continue;
        }
        if (!inputs && arg.starts_with("--timings-file=")) {
            auto const path = arg.substr(std::string_view("--timings-file=").size());
            if (path.empty()) fail("empty --timings-file");
            result.timings_file = std::string(path);
            continue;
        }
        if (!inputs && arg.starts_with("--output=")) {
            auto const path = arg.substr(std::string_view("--output=").size());
            if (path.empty()) fail("empty --output");
            result.output = std::string(path);
            continue;
        }
        if (!inputs) fail("unknown option '" + std::string(arg) + "'");
        result.bitcode_inputs.emplace_back(arg);
    }
    if (result.metadata_dir.empty()) fail("missing --metadata-dir");
    if (result.output.empty()) fail("missing --output");
    if (result.bitcode_inputs.empty()) fail("missing LLVM bitcode inputs after --");
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
    for (std::size_t i = 0; i < command.size(); ++i) {
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

struct PackageDefinitionMetadata {
    std::string id;
    std::string kind;
    std::string declaration_usr;
    std::string source_file;
    std::optional<iv::NodeCodeKey> node_code_key;
};

struct CompilerMetadata {
    std::vector<StateMetadata> states;
    std::vector<ConfigPointerMetadata> config_pointers;
    std::vector<PackageDefinitionMetadata> package_definitions;
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
        auto* package_definitions = object->getArray("package_definitions");
        if (!package_definitions) {
            fail("metadata has no registered-definition array in '"
                 + entry.path().string() + "'");
        }
        for (auto const& definition_value : *package_definitions) {
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
            PackageDefinitionMetadata registered{
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
                result.package_definitions.begin(),
                result.package_definitions.end(),
                [&](PackageDefinitionMetadata const& existing) {
                    return existing.id == registered.id
                        && existing.declaration_usr == registered.declaration_usr;
                });
            if (duplicate == result.package_definitions.end()) {
                result.package_definitions.push_back(std::move(registered));
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
                for (auto const& block : *function) {
                    for (auto const& instruction : block) {
                        for (auto const& operand : instruction.operands()) {
                            mark_reachable(operand.get(), reachable);
                        }
                    }
                }
            }
        } else if (auto const* variable = dyn_cast<GlobalVariable>(global)) {
            if (variable->hasInitializer()) mark_reachable(variable->getInitializer(), reachable);
        } else if (auto const* alias = dyn_cast<GlobalAlias>(global)) {
            mark_reachable(alias->getAliasee(), reachable);
        }
        return;
    }
    if (auto const* constant = dyn_cast<Constant>(value)) {
        for (auto const& operand : constant->operands()) {
            mark_reachable(operand.get(), reachable);
        }
    }
}


void reject_node_runtime_mutable_globals(std::span<IrNodeRecord const> records)
{
    for (auto const& record : records) {
        auto const* initializer = dyn_cast_or_null<ConstantStruct>(record.initializer);
        if (!initializer || initializer->getNumOperands() != 6) {
            fail("malformed compiler record while validating node runtime globals");
        }
        SmallPtrSet<GlobalValue const*, 64> reachable;
        mark_reachable(initializer->getOperand(1), reachable);
        for (auto const* value : reachable) {
            auto const* global = dyn_cast<GlobalVariable>(value);
            if (!global || global->isDeclaration() || global->isConstant()
                || global->getName().starts_with("llvm.")) {
                continue;
            }
            fail(
                "node type '" + record.type_name
                + "' runtime/compiler operations reference mutable package global '"
                + global->getName().str()
                + "'; persistent mutable runtime data must be Node::State");
        }
    }
}

struct RetainedGlobal {
    GlobalVariable* global = nullptr;
    std::size_t size = 0;
};

std::vector<GlobalVariable*> package_definition_globals(Module& module)
{
    std::vector<GlobalVariable*> result;
    for (auto& global : module.globals()) {
        if (global.getSection() == iv::details::package_definition_section
            && global.isConstant() && global.hasInitializer()) {
            result.push_back(&global);
        }
    }
    std::ranges::sort(result, {}, [](GlobalVariable const* definition) {
        return definition->getName();
    });
    return result;
}

std::string constant_string_field(
    Constant* value, Constant* size_value, std::string_view field)
{
    StringRef text;
    if (!getConstantStringInfo(value, text, false)) {
        fail("package definition does not contain a constant " + std::string(field));
    }
    auto const size = constant_size(size_value, field);
    if (size > text.size()) {
        fail("package definition " + std::string(field) + " length is invalid");
    }
    return text.substr(0, size).str();
}

iv::NodeCodeKey compiler_record_key(Constant* pointer)
{
    auto const* global = dyn_cast<GlobalVariable>(getUnderlyingObject(pointer));
    if (!global || !global->hasInitializer()) {
        fail("registered node type does not reference a compiler record global");
    }
    auto const* record = dyn_cast<ConstantStruct>(global->getInitializer());
    if (!record || record->getNumOperands() != 6) {
        fail("registered node type references a malformed compiler record");
    }
    auto const* key = dyn_cast<ConstantStruct>(record->getOperand(0));
    if (!key || key->getNumOperands() != 2) {
        fail("registered node type references a malformed compiler record key");
    }
    return {
        .low = constant_u64(key->getOperand(0)),
        .high = constant_u64(key->getOperand(1)),
    };
}

void validate_package_definitions(
    std::span<GlobalVariable* const> definitions,
    CompilerMetadata const& metadata)
{
    std::set<std::pair<std::string, std::string>> runtime;
    std::optional<std::string> package_root;
    for (auto* global : definitions) {
        auto* record = dyn_cast_or_null<ConstantStruct>(global->getInitializer());
        if (!record || record->getNumOperands() != 10) {
            fail("malformed IV package definition record");
        }
        auto const kind_value = constant_u64(record->getOperand(0));
        std::string kind;
        if (kind_value == static_cast<std::uint64_t>(iv::details::PackageDefinitionKind::node)) {
            kind = "node";
        } else if (kind_value == static_cast<std::uint64_t>(iv::details::PackageDefinitionKind::module)) {
            kind = "module";
        } else {
            fail("IV package definition has an invalid kind");
        }
        auto const id = constant_string_field(
            record->getOperand(1), record->getOperand(2), "stable ID");
        if (id.empty()) fail("IV package definition has an empty stable ID");
        auto const root = constant_string_field(
            record->getOperand(5), record->getOperand(6), "package root");
        if (!package_root) package_root = root;
        else if (*package_root != root) {
            fail("one IV package emitted definitions for multiple package roots");
        }
        if (!runtime.emplace(kind, id).second) {
            fail("duplicate registered IV definition ID within one IV package: '" + id + "'");
        }

        auto const compiler = std::find_if(
            metadata.package_definitions.begin(),
            metadata.package_definitions.end(),
            [&](PackageDefinitionMetadata const& definition) {
                return definition.kind == kind && definition.id == id;
            });
        if (compiler == metadata.package_definitions.end()) {
            fail("registered IV " + kind + " '" + id
                + "' has no matching compiler definition metadata");
        }
        if (kind == "node" && compiler->node_code_key) {
            auto const key = compiler_record_key(record->getOperand(9));
            if (key != *compiler->node_code_key) {
                fail("registered IV node '" + id
                    + "' compiler record does not match compiler metadata");
            }
        }
    }

    for (auto const& definition : metadata.package_definitions) {
        if (!runtime.contains({definition.kind, definition.id})) {
            fail("compiler metadata registered " + definition.kind + " '"
                + definition.id + "' but the IV package definition table did not");
        }
    }
}

std::vector<RetainedGlobal> collect_retained_globals(
    Module& module,
    std::span<GlobalVariable* const> definitions)
{
    SmallPtrSet<GlobalValue const*, 64> reachable;
    for (auto* definition : definitions) mark_reachable(definition, reachable);
    if (auto* ctors = module.getGlobalVariable("llvm.global_ctors")) {
        mark_reachable(ctors, reachable);
    }
    if (auto* dtors = module.getGlobalVariable("llvm.global_dtors")) {
        mark_reachable(dtors, reachable);
    }

    std::vector<RetainedGlobal> result;
    for (auto* value : reachable) {
        auto* global = dyn_cast<GlobalVariable>(const_cast<GlobalValue*>(value));
        if (!global || !global->isConstant() || global->isDeclaration()
            || !global->hasInitializer()
            || global->getSection() == iv::details::package_definition_section
            || global->getSection() == "iv_node_types"
            || global->getName().starts_with("llvm.")) {
            continue;
        }
        auto const size = module.getDataLayout().getTypeAllocSize(global->getValueType());
        if (size.isScalable() || size.getFixedValue() == 0) continue;
        result.push_back({.global = global, .size = size.getFixedValue()});
    }
    std::ranges::sort(result, {}, [](RetainedGlobal const& value) {
        return value.global->getName();
    });
    return result;
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


void inject_package_abi_version(Module& module)
{
    auto& context = module.getContext();
    auto* function = Function::Create(
        FunctionType::get(Type::getInt32Ty(context), false),
        GlobalValue::ExternalLinkage,
        "iv_package_abi_version",
        module);
    function->setVisibility(GlobalValue::DefaultVisibility);
    auto* block = BasicBlock::Create(context, "entry", function);
    IRBuilder<> builder(block);
    builder.CreateRet(ConstantInt::get(
        Type::getInt32Ty(context), iv::IV_PACKAGE_ABI_VERSION));
}

void inject_package_definition_table(
    Module& module,
    std::span<GlobalVariable* const> definitions)
{
    auto* pointer_type = PointerType::getUnqual(module.getContext());
    if (definitions.empty()) {
        emit_view_accessor(
            module, "iv_package_definitions",
            ConstantPointerNull::get(pointer_type), 0);
        return;
    }

    auto* record_type = definitions.front()->getValueType();
    for (auto const* definition : definitions) {
        if (definition->getValueType() != record_type) {
            fail("IV package definition records have inconsistent LLVM types");
        }
    }
    auto const record_size = module.getDataLayout().getTypeAllocSize(record_type);
    if (record_size.isScalable()
        || record_size.getFixedValue() != sizeof(iv::details::PackageDefinition)) {
        fail("IV package definition record ABI does not match PackageDefinition");
    }

    std::vector<Constant*> values;
    values.reserve(definitions.size());
    for (auto const* definition : definitions) {
        values.push_back(definition->getInitializer());
    }
    auto* array_type = ArrayType::get(record_type, values.size());
    auto* table = new GlobalVariable(
        module, array_type, true, GlobalValue::PrivateLinkage,
        ConstantArray::get(array_type, values), "iv.package_definitions");
    emit_view_accessor(
        module, "iv_package_definitions",
        ConstantExpr::getPointerCast(table, pointer_type),
        values.size() * record_size.getFixedValue());
}

Constant* string_view_constant(
    Module& module,
    StructType* view_type,
    StringRef global_name,
    std::string_view value)
{
    auto* pointer_type = PointerType::getUnqual(module.getContext());
    auto* size_type = cast<IntegerType>(view_type->getElementType(1));
    if (value.empty()) {
        return ConstantStruct::get(
            view_type,
            ConstantPointerNull::get(pointer_type),
            ConstantInt::get(size_type, 0));
    }
    auto bytes = std::as_bytes(std::span(value.data(), value.size()));
    auto* global = constant_bytes(module, global_name, bytes, 1);
    return ConstantStruct::get(
        view_type,
        ConstantExpr::getPointerCast(global, pointer_type),
        ConstantInt::get(size_type, value.size()));
}

void inject_package_configuration_metadata(
    Module& module,
    CompilerMetadata const& metadata,
    std::span<std::pair<iv::NodeCodeKey, iv::NodeStateStructure> const> state_structures,
    std::span<RetainedGlobal const> retained_globals)
{
    auto& context = module.getContext();
    auto const pointer_bits = module.getDataLayout().getPointerSizeInBits();
    auto* size_type = IntegerType::get(context, pointer_bits);
    auto* pointer_type = PointerType::getUnqual(context);
    auto* i64 = Type::getInt64Ty(context);
    auto* i8 = Type::getInt8Ty(context);
    auto* code_key_type = StructType::get(i64, i64);
    auto* view_type = StructType::get(pointer_type, size_type);

    auto code_key = [&](iv::NodeCodeKey key) -> Constant* {
        return ConstantStruct::get(
            code_key_type,
            ConstantInt::get(i64, key.low),
            ConstantInt::get(i64, key.high));
    };

    auto* pointer_field_type = StructType::get(code_key_type, size_type);
    auto const pointer_field_size = module.getDataLayout().getTypeAllocSize(pointer_field_type);
    if (pointer_field_size.isScalable()
        || pointer_field_size.getFixedValue() != sizeof(iv::NodeConfigPointerFieldData)) {
        fail("node-config pointer-field ABI does not match NodeConfigPointerFieldData");
    }
    std::vector<Constant*> pointer_fields;
    for (auto const& entry : metadata.config_pointers) {
        for (auto const offset : entry.byte_offsets) {
            pointer_fields.push_back(ConstantStruct::get(
                pointer_field_type,
                code_key(entry.key),
                ConstantInt::get(size_type, offset)));
        }
    }
    if (pointer_fields.empty()) {
        emit_view_accessor(
            module, "iv_package_node_config_pointer_fields",
            ConstantPointerNull::get(pointer_type), 0);
    } else {
        auto* array_type = ArrayType::get(pointer_field_type, pointer_fields.size());
        auto* table = new GlobalVariable(
            module, array_type, true, GlobalValue::PrivateLinkage,
            ConstantArray::get(array_type, pointer_fields),
            "iv.package_node_config_pointer_fields");
        emit_view_accessor(
            module, "iv_package_node_config_pointer_fields",
            ConstantExpr::getPointerCast(table, pointer_type),
            pointer_fields.size() * pointer_field_size.getFixedValue());
    }

    auto* retained_global_type = StructType::get(pointer_type, size_type, size_type);
    auto const retained_global_size = module.getDataLayout().getTypeAllocSize(retained_global_type);
    if (retained_global_size.isScalable()
        || retained_global_size.getFixedValue() != sizeof(iv::RetainedGlobalData)) {
        fail("retained-global ABI does not match RetainedGlobalData");
    }
    std::vector<Constant*> globals;
    globals.reserve(retained_globals.size());
    for (std::size_t ordinal = 0; ordinal < retained_globals.size(); ++ordinal) {
        globals.push_back(ConstantStruct::get(
            retained_global_type,
            ConstantExpr::getPointerCast(retained_globals[ordinal].global, pointer_type),
            ConstantInt::get(size_type, retained_globals[ordinal].size),
            ConstantInt::get(size_type, ordinal)));
    }
    if (globals.empty()) {
        emit_view_accessor(
            module, "iv_package_retained_globals",
            ConstantPointerNull::get(pointer_type), 0);
    } else {
        auto* array_type = ArrayType::get(retained_global_type, globals.size());
        auto* table = new GlobalVariable(
            module, array_type, true, GlobalValue::PrivateLinkage,
            ConstantArray::get(array_type, globals),
            "iv.package_retained_globals");
        emit_view_accessor(
            module, "iv_package_retained_globals",
            ConstantExpr::getPointerCast(table, pointer_type),
            globals.size() * retained_global_size.getFixedValue());
    }

    auto* state_field_type = StructType::get(
        view_type, view_type, size_type, size_type, size_type, size_type, i8);
    auto const state_field_size = module.getDataLayout().getTypeAllocSize(state_field_type);
    if (state_field_size.isScalable()
        || state_field_size.getFixedValue() != sizeof(iv::NodeStateFieldData)) {
        fail("node-state field ABI does not match NodeStateFieldData");
    }
    auto* state_structure_type = StructType::get(
        code_key_type, size_type, size_type, view_type);
    auto const state_structure_size = module.getDataLayout().getTypeAllocSize(state_structure_type);
    if (state_structure_size.isScalable()
        || state_structure_size.getFixedValue() != sizeof(iv::NodeStateStructureData)) {
        fail("node-state structure ABI does not match NodeStateStructureData");
    }

    std::vector<Constant*> structures;
    structures.reserve(state_structures.size());
    for (std::size_t structure_index = 0;
         structure_index < state_structures.size(); ++structure_index) {
        auto const& [key, state] = state_structures[structure_index];
        std::vector<Constant*> fields;
        fields.reserve(state.fields.size());
        for (std::size_t field_index = 0; field_index < state.fields.size(); ++field_index) {
            auto const& field = state.fields[field_index];
            auto const prefix = "iv.package_state." + std::to_string(structure_index)
                + ".field." + std::to_string(field_index);
            fields.push_back(ConstantStruct::get(
                state_field_type,
                string_view_constant(module, view_type, prefix + ".name", field.name),
                string_view_constant(module, view_type, prefix + ".type", field.type_name),
                ConstantInt::get(size_type, field.bit_offset),
                ConstantInt::get(size_type, field.size_bits),
                ConstantInt::get(size_type, field.alignment_bits),
                ConstantInt::get(size_type, field.bit_width.value_or(0)),
                ConstantInt::get(i8, field.bit_width.has_value() ? 1 : 0)));
        }

        Constant* fields_view = ConstantStruct::get(
            view_type,
            ConstantPointerNull::get(pointer_type),
            ConstantInt::get(size_type, 0));
        if (!fields.empty()) {
            auto* fields_type = ArrayType::get(state_field_type, fields.size());
            auto* fields_global = new GlobalVariable(
                module, fields_type, true, GlobalValue::PrivateLinkage,
                ConstantArray::get(fields_type, fields),
                "iv.package_state_fields." + std::to_string(structure_index));
            fields_view = ConstantStruct::get(
                view_type,
                ConstantExpr::getPointerCast(fields_global, pointer_type),
                ConstantInt::get(
                    size_type, fields.size() * state_field_size.getFixedValue()));
        }
        structures.push_back(ConstantStruct::get(
            state_structure_type,
            code_key(key),
            ConstantInt::get(size_type, state.size_bits),
            ConstantInt::get(size_type, state.alignment_bits),
            fields_view));
    }
    if (structures.empty()) {
        emit_view_accessor(
            module, "iv_package_node_state_structures",
            ConstantPointerNull::get(pointer_type), 0);
    } else {
        auto* array_type = ArrayType::get(state_structure_type, structures.size());
        auto* table = new GlobalVariable(
            module, array_type, true, GlobalValue::PrivateLinkage,
            ConstantArray::get(array_type, structures),
            "iv.package_node_state_structures");
        emit_view_accessor(
            module, "iv_package_node_state_structures",
            ConstantExpr::getPointerCast(table, pointer_type),
            structures.size() * state_structure_size.getFixedValue());
    }
}

void preserve_package_code(Module& module)
{
    static constexpr std::array<StringRef, 5> entry_points{
        "iv_package_abi_version",
        "iv_package_definitions",
        "iv_package_node_config_pointer_fields",
        "iv_package_retained_globals",
        "iv_package_node_state_structures",
    };
    SmallPtrSet<GlobalValue const*, 64> reachable;
    for (auto const name : entry_points) {
        auto* function = module.getFunction(name);
        if (!function || function->isDeclaration()) {
            fail("finalized IV package does not define '" + name.str() + "'");
        }
        mark_reachable(function, reachable);
    }
    if (auto* ctors = module.getGlobalVariable("llvm.global_ctors")) {
        mark_reachable(ctors, reachable);
    }
    if (auto* dtors = module.getGlobalVariable("llvm.global_dtors")) {
        mark_reachable(dtors, reachable);
    }

    SmallVector<GlobalValue*, 64> used;
    for (auto const* value : reachable) {
        if (!value->isDeclaration()
            && value->getName() != "llvm.used"
            && value->getName() != "llvm.compiler.used") {
            used.push_back(const_cast<GlobalValue*>(value));
        }
    }
    appendToUsed(module, used);
    legacy::PassManager passes;
    passes.add(createGlobalDCEPass());
    passes.run(module);
}


void write_package_bitcode(Module& module, std::filesystem::path const& path)
{
    auto temporary = path;
    temporary += ".tmp";
    std::error_code ec;
    raw_fd_ostream output(temporary.string(), ec, sys::fs::OF_None);
    if (ec) {
        fail("cannot create finalized IV package bitcode '" + temporary.string()
            + "': " + ec.message());
    }
    WriteBitcodeToFile(module, output);
    output.flush();
    if (output.has_error()) {
        fail("cannot finish finalized IV package bitcode '" + temporary.string() + "'");
    }
    output.close();
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            fail("cannot publish finalized IV package bitcode '" + path.string()
                + "': " + ec.message());
        }
    }
}

int finalize(Options options)
{
    TimingReport timings;
    auto stage_started_at = timings.start_stage();
    options.bitcode_inputs = expand_response_files(std::move(options.bitcode_inputs));
    LLVMContext context;
    auto linked = link_bitcode_inputs(options.bitcode_inputs, context);
    timings.finish_stage("bitcode_parse_link", stage_started_at);

    auto& package = *linked.module;
    if (package.getDataLayout().isDefault()) {
        fail("linked IV package LLVM has no target data layout");
    }

    stage_started_at = timings.start_stage();
    auto node_records = scan_node_records(package);
    auto metadata = load_metadata(options.metadata_dir);
    reject_node_runtime_mutable_globals(node_records);
    auto state_structures = bind_state_metadata(node_records, metadata);
    require_node_config_metadata(node_records, metadata);
    auto definitions = package_definition_globals(package);
    validate_package_definitions(definitions, metadata);
    auto retained_globals = collect_retained_globals(package, definitions);
    timings.finish_stage("package_metadata_validate", stage_started_at);

    stage_started_at = timings.start_stage();
    inject_package_abi_version(package);
    inject_package_definition_table(package, definitions);
    inject_package_configuration_metadata(
        package, metadata, state_structures, retained_globals);
    preserve_package_code(package);
    timings.finish_stage("package_metadata_inject", stage_started_at);

    stage_started_at = timings.start_stage();
    write_package_bitcode(package, options.output);
    timings.finish_stage("package_bitcode_write", stage_started_at);

    if (options.timings_file) timings.write(*options.timings_file);
    return 0;
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
