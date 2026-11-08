#include <intravenous/module/abi.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/node/node_state_structure.h>
#include <intravenous/node/presentation.h>

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
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/Archive.h"
#include "llvm/Pass.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
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
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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
        output << "version=0\n";
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

bool try_parse_bitcode(
    MemoryBufferRef buffer,
    LLVMContext& context,
    std::unique_ptr<Module>& output)
{
    auto module = parseBitcodeFile(buffer, context);
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

void link_input_module(
    LinkedModule& result,
    std::unique_ptr<Module> input,
    std::filesystem::path const& input_path)
{
    if (!result.module) {
        result.module = std::move(input);
    } else if (Linker::linkModules(*result.module, std::move(input))) {
        fail("LLVM link failed while combining '" + input_path.string() + "'");
    }
}

void link_bitcode_archive(
    std::filesystem::path const& path,
    LLVMContext& context,
    LinkedModule& result)
{
    auto buffer = MemoryBuffer::getFile(path.string());
    if (!buffer) {
        fail("cannot read LLVM archive '" + path.string() + "': "
             + buffer.getError().message());
    }
    auto archive = object::Archive::create((*buffer)->getMemBufferRef());
    if (!archive) {
        fail("cannot read LLVM archive '" + path.string() + "': "
             + error_string(archive.takeError()));
    }

    Error child_error = Error::success();
    std::size_t member_count = 0;
    for (auto const& child : (*archive)->children(child_error)) {
        auto member_buffer = child.getMemoryBufferRef();
        if (!member_buffer) {
            fail("cannot read member of LLVM archive '" + path.string() + "': "
                 + error_string(member_buffer.takeError()));
        }
        std::unique_ptr<Module> input;
        if (!try_parse_bitcode(*member_buffer, context, input)) continue;
        ++member_count;
        link_input_module(result, std::move(input), path);
    }
    if (child_error) {
        fail("cannot enumerate LLVM archive '" + path.string() + "': "
             + error_string(std::move(child_error)));
    }
    if (member_count == 0) {
        fail("LLVM archive '" + path.string() + "' contains no LLVM bitcode members");
    }
    result.bitcode_inputs.push_back(std::filesystem::absolute(path));
}

LinkedModule link_bitcode_inputs(
    std::span<std::string const> command,
    LLVMContext& context)
{
    LinkedModule result;
    for (std::size_t i = 0; i < command.size(); ++i) {
        std::filesystem::path path(command[i]);
        if (command[i].empty() || command[i][0] == '-' || !std::filesystem::is_regular_file(path)) continue;
        auto const extension = path.extension().string();
        if (extension == ".a" || extension == ".lib") {
            link_bitcode_archive(path, context, result);
            continue;
        }
        std::unique_ptr<Module> input;
        if (!try_parse_bitcode(path, context, input)) continue;
        link_input_module(result, std::move(input), path);
        result.bitcode_inputs.push_back(std::filesystem::absolute(path));
    }
    if (!result.module) fail("link command contains no LLVM bitcode object inputs");
    return result;
}

struct IrNodeRecord {
    iv::NodeCodeKey key{};
    std::string type_name{};
    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
    std::size_t background_state_size = 0;
    std::size_t background_state_alignment = 1;
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
        if (!record || record->getNumOperands() != 11) fail("malformed iv_node_types record");
        auto* key = dyn_cast<ConstantStruct>(record->getOperand(0));
        if (!key || key->getNumOperands() != 2) fail("malformed iv_node_types key");
        auto* operations = dyn_cast<ConstantStruct>(record->getOperand(1));
        if (!operations || operations->getNumOperands() != 13) {
            fail("malformed iv_node_types operations ABI");
        }
        StringRef name;
        if (!getConstantStringInfo(record->getOperand(2), name, false))
            fail("node compiler record does not contain a constant type name");
        auto const name_size = constant_size(record->getOperand(3), "type name size");
        if (name_size > name.size()) fail("node compiler record type name length is invalid");
        auto const replay_marker = constant_u64(record->getOperand(8));
        if (replay_marker > 1) {
            fail("malformed iv_node_types intrinsic replayability flag");
        }
        auto const pointwise_marker = constant_u64(record->getOperand(9));
        if (pointwise_marker > 1) {
            fail("malformed iv_node_types pointwise Tick flag");
        }
        auto const block_input_marker = constant_u64(record->getOperand(10));
        if (block_input_marker > 1) {
            fail("malformed iv_node_types block input access flag");
        }
        result.push_back({
            .key = {
                .low = constant_u64(key->getOperand(0)),
                .high = constant_u64(key->getOperand(1)),
            },
            .type_name = name.substr(0, name_size).str(),
            .state_size = constant_size(record->getOperand(4), "state size"),
            .state_alignment = constant_size(record->getOperand(5), "state alignment"),
            .background_state_size = constant_size(
                record->getOperand(6), "background state size"),
            .background_state_alignment = constant_size(
                record->getOperand(7), "background state alignment"),
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
    iv::NodeStateStructure structure{};
};

struct ConfigPointerMetadata {
    iv::NodeCodeKey key{};
    std::vector<std::size_t> byte_offsets{};
};

struct ConfigurationSchemaMetadata;

struct ConfigurationFieldMetadata {
    std::string name;
    std::size_t bit_offset = 0;
    std::string assign_symbol;
    std::unique_ptr<ConfigurationSchemaMetadata> type;
};

struct ConfigurationEnumeratorMetadata {
    std::string name;
    std::int64_t signed_value = 0;
    std::uint64_t unsigned_value = 0;
};

struct ConfigurationSchemaMetadata {
    std::string kind;
    std::string display_name;
    std::size_t size_bits = 0;
    std::size_t alignment_bits = 0;
    bool integer_is_signed = false;
    std::size_t integer_bit_width = 0;
    std::string construct_scalar_symbol;
    std::string read_scalar_symbol;
    std::string default_construct_symbol;
    std::string destroy_symbol;
    std::string element_assign_symbol;
    std::string append_element_symbol;
    std::string element_count_symbol;
    std::string element_at_symbol;
    std::string construct_node_address_symbol;
    std::size_t fixed_element_count = 0;
    std::vector<ConfigurationFieldMetadata> fields;
    std::vector<ConfigurationEnumeratorMetadata> enumerators;
    std::unique_ptr<ConfigurationSchemaMetadata> element_type;
};

struct ConfigurationTypeMetadata {
    std::string symbol;
    std::string nominal_id;
    std::string definition_fingerprint;
    std::string display_name;
    ConfigurationSchemaMetadata expression_schema;
};

struct PackageDefinitionMetadata {
    std::string id;
    std::string kind;
    std::string declaration_usr;
    std::string source_file;
    std::optional<iv::NodeCodeKey> node_code_key;
};

struct NodeMessageHandlerMetadata {
    enum class OwnerKind {
        node,
        module,
    };

    OwnerKind owner_kind = OwnerKind::node;
    std::optional<iv::NodeCodeKey> node_code_key{};
    std::string definition_id{};
    std::string method_symbol;
    std::string event_type_symbol;
};

struct CompilerMetadata {
    std::vector<StateMetadata> states;
    std::vector<StateMetadata> background_states;
    std::vector<ConfigPointerMetadata> config_pointers;
    std::vector<ConfigurationTypeMetadata> configuration_types;
    std::vector<NodeMessageHandlerMetadata> node_message_handlers;
    std::vector<PackageDefinitionMetadata> package_definitions;
};

std::string read_file(std::filesystem::path const& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("cannot read metadata file '" + path.string() + "'");
    return {std::istreambuf_iterator<char>(stream), {}};
}

ConfigurationSchemaMetadata parse_configuration_schema(
    json::Object const& object,
    std::filesystem::path const& metadata_path)
{
    auto kind = object.getString("kind");
    auto display_name = object.getString("display_name");
    auto size_bits = object.getInteger("size_bits");
    auto alignment_bits = object.getInteger("alignment_bits");
    auto destroy_symbol = object.getString("destroy_symbol");
    if (!kind || kind->empty() || !display_name || display_name->empty()
        || !size_bits || !alignment_bits || !destroy_symbol) {
        fail("incomplete configuration expression schema in '"
             + metadata_path.string() + "'");
    }
    ConfigurationSchemaMetadata result;
    result.kind = kind->str();
    result.display_name = display_name->str();
    result.size_bits = metadata_size(
        *size_bits, metadata_path, "configuration schema size_bits");
    result.alignment_bits = metadata_size(
        *alignment_bits, metadata_path, "configuration schema alignment_bits");
    result.destroy_symbol = destroy_symbol->str();
    if (auto value = object.getBoolean("integer_is_signed"))
        result.integer_is_signed = *value;
    if (auto value = object.getInteger("integer_bit_width")) {
        result.integer_bit_width = metadata_size(
            *value, metadata_path, "configuration schema integer_bit_width");
    }
    if (auto value = object.getString("construct_scalar_symbol"))
        result.construct_scalar_symbol = value->str();
    if (auto value = object.getString("read_scalar_symbol"))
        result.read_scalar_symbol = value->str();
    if (auto value = object.getString("default_construct_symbol"))
        result.default_construct_symbol = value->str();
    if (auto value = object.getString("element_assign_symbol"))
        result.element_assign_symbol = value->str();
    if (auto value = object.getString("append_element_symbol"))
        result.append_element_symbol = value->str();
    if (auto value = object.getString("element_count_symbol"))
        result.element_count_symbol = value->str();
    if (auto value = object.getString("element_at_symbol"))
        result.element_at_symbol = value->str();
    if (auto value = object.getString("construct_node_address_symbol"))
        result.construct_node_address_symbol = value->str();
    if (auto value = object.getInteger("fixed_element_count")) {
        result.fixed_element_count = metadata_size(
            *value, metadata_path, "configuration schema fixed_element_count");
    }
    if (auto fields = object.getArray("fields")) {
        for (auto const& field_value : *fields) {
            auto const* field = field_value.getAsObject();
            if (!field) fail("configuration schema field is not an object in '"
                             + metadata_path.string() + "'");
            auto name = field->getString("name");
            auto bit_offset = field->getInteger("bit_offset");
            auto assign_symbol = field->getString("assign_symbol");
            auto const* type = field->getObject("type");
            if (!name || name->empty() || !bit_offset || !assign_symbol || !type) {
                fail("incomplete configuration schema field in '"
                     + metadata_path.string() + "'");
            }
            result.fields.push_back(ConfigurationFieldMetadata{
                .name = name->str(),
                .bit_offset = metadata_size(
                    *bit_offset, metadata_path, "configuration field bit_offset"),
                .assign_symbol = assign_symbol->str(),
                .type = std::make_unique<ConfigurationSchemaMetadata>(
                    parse_configuration_schema(*type, metadata_path)),
            });
        }
    }
    if (auto enumerators = object.getArray("enumerators")) {
        for (auto const& enumerator_value : *enumerators) {
            auto const* enumerator = enumerator_value.getAsObject();
            if (!enumerator) {
                fail("configuration enumerator is not an object in '"
                     + metadata_path.string() + "'");
            }
            auto name = enumerator->getString("name");
            if (!name || name->empty()) {
                fail("configuration enumerator has no name in '"
                     + metadata_path.string() + "'");
            }
            ConfigurationEnumeratorMetadata item{.name = name->str()};
            if (auto value = enumerator->getInteger("signed_value"))
                item.signed_value = *value;
            if (auto value = enumerator->getString("unsigned_value")) {
                auto parsed = std::from_chars(
                    value->begin(), value->end(), item.unsigned_value);
                if (parsed.ec != std::errc{} || parsed.ptr != value->end()) {
                    fail("configuration enumerator has an invalid unsigned value in '"
                         + metadata_path.string() + "'");
                }
            }
            result.enumerators.push_back(std::move(item));
        }
    }
    if (auto const* element = object.getObject("element_type")) {
        result.element_type = std::make_unique<ConfigurationSchemaMetadata>(
            parse_configuration_schema(*element, metadata_path));
    }
    return result;
}

bool same_configuration_schema(
    ConfigurationSchemaMetadata const& lhs,
    ConfigurationSchemaMetadata const& rhs)
{
    if (lhs.kind != rhs.kind || lhs.display_name != rhs.display_name
        || lhs.size_bits != rhs.size_bits
        || lhs.alignment_bits != rhs.alignment_bits
        || lhs.integer_is_signed != rhs.integer_is_signed
        || lhs.integer_bit_width != rhs.integer_bit_width
        || lhs.construct_scalar_symbol != rhs.construct_scalar_symbol
        || lhs.read_scalar_symbol != rhs.read_scalar_symbol
        || lhs.default_construct_symbol != rhs.default_construct_symbol
        || lhs.destroy_symbol != rhs.destroy_symbol
        || lhs.element_assign_symbol != rhs.element_assign_symbol
        || lhs.append_element_symbol != rhs.append_element_symbol
        || lhs.element_count_symbol != rhs.element_count_symbol
        || lhs.element_at_symbol != rhs.element_at_symbol
        || lhs.construct_node_address_symbol
            != rhs.construct_node_address_symbol
        || lhs.fixed_element_count != rhs.fixed_element_count
        || lhs.fields.size() != rhs.fields.size()
        || lhs.enumerators.size() != rhs.enumerators.size()
        || static_cast<bool>(lhs.element_type)
            != static_cast<bool>(rhs.element_type)) {
        return false;
    }
    if (lhs.element_type
        && !same_configuration_schema(*lhs.element_type, *rhs.element_type)) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.fields.size(); ++index) {
        auto const& a = lhs.fields[index];
        auto const& b = rhs.fields[index];
        if (a.name != b.name || a.bit_offset != b.bit_offset
            || a.assign_symbol != b.assign_symbol || !a.type || !b.type
            || !same_configuration_schema(*a.type, *b.type)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < lhs.enumerators.size(); ++index) {
        auto const& a = lhs.enumerators[index];
        auto const& b = rhs.enumerators[index];
        if (a.name != b.name || a.signed_value != b.signed_value
            || a.unsigned_value != b.unsigned_value) {
            return false;
        }
    }
    return true;
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
        auto* states = object->getArray("states");
        if (!states) {
            fail("metadata has no state array in '" + entry.path().string() + "'");
        }
        auto* background_states = object->getArray("background_states");
        if (!background_states) {
            fail("metadata has no background-state array in '" + entry.path().string() + "'");
        }
        auto* config_pointers = object->getArray("config_pointers");
        if (!config_pointers) {
            fail("metadata has no config-pointer array in '" + entry.path().string() + "'");
        }
        auto* configuration_types = object->getArray("configuration_types");
        if (!configuration_types) {
            fail("metadata has no configuration-type array in '"
                 + entry.path().string() + "'");
        }
        auto* node_message_handlers = object->getArray("node_message_handlers");
        if (!node_message_handlers) {
            fail("metadata has no node-message-handler array in '"
                 + entry.path().string() + "'");
        }
        auto* package_definitions = object->getArray("package_definitions");
        if (!package_definitions) {
            fail("metadata has no package-definition array in '"
                 + entry.path().string() + "'");
        }
        for (auto const& type_value : *configuration_types) {
            auto* type = type_value.getAsObject();
            if (!type) {
                fail("configuration-type metadata entry is not an object in '"
                     + entry.path().string() + "'");
            }
            auto symbol = type->getString("symbol");
            auto nominal_id = type->getString("nominal_id");
            auto fingerprint = type->getString("definition_fingerprint");
            auto display_name = type->getString("display_name");
            auto* expression_schema = type->getObject("expression_schema");
            if (!symbol || symbol->empty() || !nominal_id || nominal_id->empty()
                || !fingerprint || fingerprint->empty()
                || !display_name || display_name->empty() || !expression_schema) {
                fail("incomplete configuration-type metadata entry in '"
                     + entry.path().string() + "'");
            }
            ConfigurationTypeMetadata metadata_type{
                .symbol = symbol->str(),
                .nominal_id = nominal_id->str(),
                .definition_fingerprint = fingerprint->str(),
                .display_name = display_name->str(),
                .expression_schema = parse_configuration_schema(
                    *expression_schema, entry.path()),
            };
            auto const duplicate = std::find_if(
                result.configuration_types.begin(),
                result.configuration_types.end(),
                [&](ConfigurationTypeMetadata const& existing) {
                    return existing.symbol == metadata_type.symbol;
                });
            if (duplicate == result.configuration_types.end()) {
                result.configuration_types.push_back(std::move(metadata_type));
            } else if (duplicate->nominal_id != metadata_type.nominal_id
                || duplicate->definition_fingerprint
                    != metadata_type.definition_fingerprint
                || duplicate->display_name != metadata_type.display_name
                || !same_configuration_schema(
                    duplicate->expression_schema,
                    metadata_type.expression_schema)) {
                fail("conflicting configuration-type metadata for LLVM symbol '"
                     + duplicate->symbol + "'");
            }
        }
        for (auto const& handler_value : *node_message_handlers) {
            auto const* handler = handler_value.getAsObject();
            auto kind = handler ? handler->getString("kind") : std::nullopt;
            auto method_symbol = handler
                ? handler->getString("method_symbol") : std::nullopt;
            auto event_type_symbol = handler
                ? handler->getString("event_type_symbol") : std::nullopt;
            if (!handler || !kind
                || (kind->str() != "node" && kind->str() != "module")
                || !method_symbol || method_symbol->empty()
                || !event_type_symbol || event_type_symbol->empty()) {
                fail("incomplete node-message-handler metadata in '"
                     + entry.path().string() + "'");
            }
            NodeMessageHandlerMetadata metadata_handler{
                .owner_kind = kind->str() == "node"
                    ? NodeMessageHandlerMetadata::OwnerKind::node
                    : NodeMessageHandlerMetadata::OwnerKind::module,
                .method_symbol = method_symbol->str(),
                .event_type_symbol = event_type_symbol->str(),
            };
            if (metadata_handler.owner_kind
                == NodeMessageHandlerMetadata::OwnerKind::node) {
                auto const* key = handler->getObject("node_code_key");
                auto low = key ? key->getString("low") : std::nullopt;
                auto high = key ? key->getString("high") : std::nullopt;
                if (!key || !low || !high) {
                    fail("node message handler has no NodeCodeKey in '"
                         + entry.path().string() + "'");
                }
                metadata_handler.node_code_key = iv::NodeCodeKey{
                    .low = parse_hex_u64(
                        *low, entry.path(), "node message NodeCodeKey.low"),
                    .high = parse_hex_u64(
                        *high, entry.path(), "node message NodeCodeKey.high"),
                };
            } else {
                auto definition_id = handler->getString("definition_id");
                if (!definition_id || definition_id->empty()) {
                    fail("module message handler has no definition ID in '"
                         + entry.path().string() + "'");
                }
                metadata_handler.definition_id = definition_id->str();
            }
            auto const duplicate = std::ranges::find_if(
                result.node_message_handlers,
                [&](NodeMessageHandlerMetadata const& existing) {
                    return existing.owner_kind == metadata_handler.owner_kind
                        && existing.node_code_key == metadata_handler.node_code_key
                        && existing.definition_id == metadata_handler.definition_id
                        && existing.method_symbol == metadata_handler.method_symbol
                        && existing.event_type_symbol
                            == metadata_handler.event_type_symbol;
                });
            if (duplicate == result.node_message_handlers.end()) {
                result.node_message_handlers.push_back(
                    std::move(metadata_handler));
            }
        }
        for (auto const& definition_value : *package_definitions) {
            auto* definition_object = definition_value.getAsObject();
            if (!definition_object) {
                fail("package-definition metadata entry is not an object in '"
                     + entry.path().string() + "'");
            }
            auto id = definition_object->getString("id");
            auto kind = definition_object->getString("kind");
            auto declaration_usr = definition_object->getString("declaration_usr");
            if (!id || id->empty() || !kind || !declaration_usr
                || (kind->str() != "node" && kind->str() != "module")) {
                fail("incomplete package-definition metadata entry in '"
                     + entry.path().string() + "'");
            }
            PackageDefinitionMetadata metadata_definition{
                .id = id->str(),
                .kind = kind->str(),
                .declaration_usr = declaration_usr->str(),
                .source_file = {},
                .node_code_key = {},
            };
            if (auto source_file = definition_object->getString("source_file")) {
                metadata_definition.source_file = source_file->str();
            }
            if (metadata_definition.kind == "node") {
                auto* key = definition_object->getObject("node_code_key");
                if (!key) {
                    fail("node package-definition metadata has no NodeCodeKey in '"
                         + entry.path().string() + "'");
                }
                auto low = key->getString("low");
                auto high = key->getString("high");
                if (!low || !high) {
                    fail("node package-definition metadata has invalid NodeCodeKey in '"
                         + entry.path().string() + "'");
                }
                metadata_definition.node_code_key = {
                    .low = parse_hex_u64(*low, entry.path(), "package definition node_code_key.low"),
                    .high = parse_hex_u64(*high, entry.path(), "package definition node_code_key.high"),
                };
            }
            auto const duplicate = std::find_if(
                result.package_definitions.begin(),
                result.package_definitions.end(),
                [&](PackageDefinitionMetadata const& existing) {
                    return existing.id == metadata_definition.id
                        && existing.declaration_usr == metadata_definition.declaration_usr;
                });
            if (duplicate == result.package_definitions.end()) {
                result.package_definitions.push_back(std::move(metadata_definition));
            }
        }
        auto append_state_metadata = [&](llvm::json::Array const& source,
                                         std::vector<StateMetadata>& destination,
                                         std::string_view label) {
            for (auto const& state_value : source) {
                auto* state = state_value.getAsObject();
                if (!state) {
                    fail(std::string(label) + " metadata entry is not an object in '"
                         + entry.path().string() + "'");
                }
                auto* key = state->getObject("node_code_key");
                auto node_type_usr = state->getString("node_type_usr");
                auto type_usr_value = state->getString("type_usr");
                auto type_name = state->getString("type");
                auto fingerprint = state->getString("definition_fingerprint");
                auto size_bits = state->getInteger("size_bits");
                auto alignment_bits = state->getInteger("alignment_bits");
                auto* fields = state->getArray("fields");
                if (!key || !node_type_usr || !type_usr_value
                    || type_usr_value->empty() || !type_name || type_name->empty()
                    || !fingerprint || fingerprint->empty() || !size_bits
                    || !alignment_bits || !fields) {
                    fail("incomplete " + std::string(label) + " metadata entry in '"
                         + entry.path().string() + "'");
                }
                auto low = key->getString("low");
                auto high = key->getString("high");
                if (!low || !high) {
                    fail(std::string(label) + " metadata has no NodeCodeKey in '"
                         + entry.path().string() + "'");
                }
                StateMetadata metadata{
                    .key = {
                        .low = parse_hex_u64(*low, entry.path(), "node_code_key.low"),
                        .high = parse_hex_u64(*high, entry.path(), "node_code_key.high"),
                    },
                    .node_type_usr = node_type_usr->str(),
                    .structure = {
                        .type_identity = {
                            .nominal_id = type_usr_value->str(),
                            .definition_fingerprint = fingerprint->str(),
                            .display_name = type_name->str(),
                        },
                        .size_bits = metadata_size(
                            *size_bits, entry.path(), std::string(label) + " size_bits"),
                        .alignment_bits = metadata_size(
                            *alignment_bits, entry.path(), std::string(label) + " alignment_bits"),
                    },
                };
                for (auto const& field_value : *fields) {
                    auto* field = field_value.getAsObject();
                    if (!field) {
                        fail(std::string(label) + " field metadata is not an object in '"
                             + entry.path().string() + "'");
                    }
                    auto name = field->getString("name");
                    auto nominal_id = field->getString("nominal_id");
                    auto field_type_usr = field->getString("type_usr");
                    auto bit_offset = field->getInteger("bit_offset");
                    auto field_size = field->getInteger("size_bits");
                    auto field_alignment = field->getInteger("alignment_bits");
                    if (!name || !nominal_id || nominal_id->empty()
                        || !field_type_usr || !bit_offset || !field_size
                        || !field_alignment) {
                        fail("incomplete " + std::string(label)
                             + " field metadata in '" + entry.path().string() + "'");
                    }
                    iv::NodeStateFieldStructure item{
                        .name = name->str(),
                        .nominal_id = nominal_id->str(),
                        .type_name = field_type_usr->str(),
                        .bit_offset = metadata_size(
                            *bit_offset, entry.path(), "field bit_offset"),
                        .size_bits = metadata_size(
                            *field_size, entry.path(), "field size_bits"),
                        .alignment_bits = metadata_size(
                            *field_alignment, entry.path(), "field alignment_bits"),
                    };
                    if (auto width = field->getInteger("bit_width")) {
                        item.bit_width = metadata_size(
                            *width, entry.path(), "field bit_width");
                    }
                    metadata.structure.fields.push_back(std::move(item));
                }
                auto const duplicate = std::find_if(
                    destination.begin(), destination.end(),
                    [&](StateMetadata const& existing) {
                        return existing.key == metadata.key;
                    });
                if (duplicate != destination.end()) {
                    if (duplicate->node_type_usr != metadata.node_type_usr
                        || duplicate->structure != metadata.structure) {
                        fail("conflicting " + std::string(label)
                             + " metadata for one NodeCodeKey in '"
                             + entry.path().string() + "'");
                    }
                    continue;
                }
                destination.push_back(std::move(metadata));
            }
        };
        append_state_metadata(*states, result.states, "state");
        append_state_metadata(
            *background_states, result.background_states, "background-state");
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
    // node_layout_type_token<T>() deliberately uses the address of writable
    // storage as a process-local type identity. Its value is never state and
    // its dedicated section prevents this narrow exemption from masking
    // ordinary mutable package globals.
    auto const is_layout_type_token = [](GlobalVariable const& global) {
        return global.getSection() == "iv.node_layout_type_tokens"
            && global.getName().contains("node_layout_type_token");
    };
    for (auto const& record : records) {
        auto const* initializer = dyn_cast_or_null<ConstantStruct>(record.initializer);
        if (!initializer || initializer->getNumOperands() != 11) {
            fail("malformed compiler record while validating node runtime globals");
        }
        SmallPtrSet<GlobalValue const*, 32> reachable;
        mark_reachable(initializer->getOperand(1), reachable);
        for (auto const* value : reachable) {
            auto const* global = dyn_cast<GlobalVariable>(value);
            if (!global || global->isDeclaration() || global->isConstant()
                || global->getName().starts_with("llvm.")
                || is_layout_type_token(*global)) {
                continue;
            }
            fail(
                "node type '" + record.type_name
                + "' runtime/compiler operations reference mutable package global '"
                + global->getName().str()
                + "'; persistent mutable runtime data must be Node::State or Node::TockState");
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
        if (global.getSection()
                == StringRef(
                    iv::details::package_definition_section.data(),
                    iv::details::package_definition_section.size())
            && global.isConstant() && global.hasInitializer()) {
            result.push_back(&global);
        }
    }
    std::ranges::sort(result, {}, [](GlobalVariable const* definition) {
        return definition->getName();
    });
    return result;
}

std::vector<GlobalVariable*> package_node_presentation_globals(Module& module)
{
    std::vector<GlobalVariable*> result;
    for (auto& global : module.globals()) {
        if (global.getSection()
                == StringRef(
                    iv::details::package_node_presentation_section.data(),
                    iv::details::package_node_presentation_section.size())
            && global.isConstant() && global.hasInitializer()) {
            result.push_back(&global);
        }
    }
    std::ranges::sort(result, {}, [](GlobalVariable const* presentation) {
        return presentation->getName();
    });
    return result;
}

std::string constant_string_field(
    Constant* value, Constant* size_value, std::string_view field)
{
    StringRef text;
    if (!getConstantStringInfo(value, text, false)) {
        fail("IV package record does not contain a constant " + std::string(field));
    }
    auto const size = constant_size(size_value, field);
    if (size > text.size()) {
        fail("IV package record " + std::string(field) + " length is invalid");
    }
    return text.substr(0, size).str();
}

iv::NodeCodeKey compiler_record_key(Constant* pointer)
{
    auto const* global = dyn_cast<GlobalVariable>(getUnderlyingObject(pointer));
    if (!global || !global->hasInitializer()) {
        fail("node type definition does not reference a compiler record global");
    }
    auto const* record = dyn_cast<ConstantStruct>(global->getInitializer());
    if (!record || record->getNumOperands() != 11) {
        fail("node type definition references a malformed compiler record");
    }
    auto const* key = dyn_cast<ConstantStruct>(record->getOperand(0));
    if (!key || key->getNumOperands() != 2) {
        fail("node type definition references a malformed compiler record key");
    }
    return {
        .low = constant_u64(key->getOperand(0)),
        .high = constant_u64(key->getOperand(1)),
    };
}

Function const* definition_callback(Constant* pointer, std::string_view field)
{
    auto const* function = dyn_cast<Function>(getUnderlyingObject(pointer));
    if (!function || function->isDeclaration()) {
        fail("IV package definition has an invalid " + std::string(field)
             + " callback");
    }
    return function;
}

void validate_package_definitions(
    std::span<GlobalVariable* const> definitions,
    CompilerMetadata const& metadata)
{
    // Node types and iv modules deliberately occupy one stable-ID namespace.
    // A duplicate in a single package is a package build error, not a later
    // cross-package registry conflict.
    std::set<std::string> runtime_ids;
    std::set<std::pair<std::string, std::string>> runtime_definitions;
    for (auto* global : definitions) {
        auto* record = dyn_cast_or_null<ConstantStruct>(global->getInitializer());
        if (!record || record->getNumOperands() != 11) {
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
        if (!runtime_ids.insert(id).second) {
            fail("duplicate IV package definition ID within one IV package: '"
                 + id + "'");
        }
        runtime_definitions.emplace(kind, id);
        if (isa<ConstantPointerNull>(record->getOperand(10))) {
            fail("IV " + kind + " '" + id
                + "' has no compiler-generated construction signature");
        }
        if (kind == "node") {
            if (isa<ConstantPointerNull>(record->getOperand(8))) {
                fail("IV node '" + id + "' has no compiler-generated constructor adapter");
            }
            auto const* constructor = definition_callback(
                record->getOperand(8), "node constructor adapter");
            auto const* signature = definition_callback(
                record->getOperand(10), "node construction signature");
            if (constructor->getName().contains("iv_package_node_configuration_pending_")
                || signature->getName().contains("iv_package_node_signature_pending_")) {
                fail("IV node '" + id
                    + "' was not processed by the IV Clang registration plugin");
            }
        } else {
            if (isa<ConstantPointerNull>(record->getOperand(7))) {
                fail("IV module '" + id + "' has no configuration callback");
            }
            auto const* signature = definition_callback(
                record->getOperand(10), "construction signature");
            auto const* configuration = definition_callback(
                record->getOperand(7), "module configuration");
            if (configuration->getName().contains(
                    "iv_package_module_class_configuration_pending")
                || signature->getName().contains(
                    "iv_package_module_class_signature_pending")) {
                fail("IV module class '" + id
                    + "' was not processed by the IV Clang registration plugin");
            }
        }

        auto const compiler = std::find_if(
            metadata.package_definitions.begin(),
            metadata.package_definitions.end(),
            [&](PackageDefinitionMetadata const& definition) {
                return definition.kind == kind && definition.id == id;
            });
        if (compiler == metadata.package_definitions.end()) {
            fail("IV " + kind + " '" + id
                + "' has no matching compiler definition metadata");
        }
        if (kind == "node" && compiler->node_code_key) {
            auto const key = compiler_record_key(record->getOperand(9));
            if (key != *compiler->node_code_key) {
                fail("IV node '" + id
                    + "' compiler record does not match compiler metadata");
            }
        }
    }

    for (auto const& definition : metadata.package_definitions) {
        if (!runtime_definitions.contains({definition.kind, definition.id})) {
            fail("compiler metadata describes " + definition.kind + " '"
                + definition.id + "' but the IV package definition table did not");
        }
    }
    for (auto const& handler : metadata.node_message_handlers) {
        if (handler.owner_kind
                == NodeMessageHandlerMetadata::OwnerKind::module
            && !runtime_definitions.contains({"module", handler.definition_id})) {
            fail("module message handler references unavailable IV module '"
                + handler.definition_id + "'");
        }
    }
}

void validate_package_node_presentations(
    std::span<GlobalVariable* const> presentations,
    std::span<GlobalVariable* const> definitions)
{
    std::set<std::string> definition_ids;
    for (auto* global : definitions) {
        auto* record = dyn_cast_or_null<ConstantStruct>(global->getInitializer());
        if (!record || record->getNumOperands() != 11) {
            fail("malformed IV package definition record");
        }
        definition_ids.insert(constant_string_field(
            record->getOperand(1), record->getOperand(2), "stable ID"));
    }

    std::set<std::pair<std::string, std::string>> registrations;
    for (auto* global : presentations) {
        auto* record = dyn_cast_or_null<ConstantStruct>(global->getInitializer());
        if (!record || record->getNumOperands() != 6) {
            fail("malformed IV package node presentation record");
        }
        auto const definition_id = constant_string_field(
            record->getOperand(0), record->getOperand(1),
            "node presentation definition ID");
        auto const presentation_id = constant_string_field(
            record->getOperand(2), record->getOperand(3),
            "node presentation ID");
        (void)constant_string_field(
            record->getOperand(4), record->getOperand(5),
            "node presentation display name");
        if (definition_id.empty()) {
            fail("IV package node presentation has an empty definition ID");
        }
        if (presentation_id.empty()) {
            fail("IV package node presentation for '" + definition_id
                + "' has an empty presentation ID");
        }
        if (!definition_ids.contains(definition_id)) {
            fail("IV package node presentation for '" + definition_id
                + "' does not name an IV_NODE or IV_MODULE in the same package");
        }
        if (!registrations.emplace(definition_id, presentation_id).second) {
            fail("duplicate IV package node presentation ('" + definition_id
                + "', '" + presentation_id + "')");
        }
    }
}

std::vector<RetainedGlobal> collect_retained_globals(
    Module& module,
    std::span<GlobalVariable* const> definitions)
{
    SmallPtrSet<GlobalValue const*, 32> reachable;
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
            || global->getSection()
                == StringRef(
                    iv::details::package_definition_section.data(),
                    iv::details::package_definition_section.size())
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
    std::span<StateMetadata const> metadata,
    bool background_state)
{
    std::vector<std::pair<iv::NodeCodeKey, iv::NodeStateStructure>> result;
    auto const label = background_state ? "Node::TockState" : "Node::State";
    for (auto const& record : records) {
        auto const state = std::find_if(metadata.begin(), metadata.end(), [&](auto const& item) {
            return item.key == record.key;
        });
        auto const state_size = background_state
            ? record.background_state_size
            : record.state_size;
        auto const state_alignment = background_state
            ? record.background_state_alignment
            : record.state_alignment;
        if (state_size == 0) {
            if (state != metadata.end()) {
                fail(std::string(label) + " metadata names a node without "
                     + label + ": '" + record.type_name + "'");
            }
            continue;
        }
        if (state_size > std::numeric_limits<std::size_t>::max() / 8
            || state_alignment > std::numeric_limits<std::size_t>::max() / 8) {
            fail(std::string(label) + " ABI size overflows for '"
                 + record.type_name + "'");
        }
        if (state == metadata.end()) {
            fail("missing NodeCodeKey-bound " + std::string(label)
                 + " metadata for '" + record.type_name + "'");
        }
        if (!state->structure.type_identity.valid()) {
            fail(std::string(label) + " type identity is incomplete for '"
                 + record.type_name + "'");
        }
        if (state->structure.size_bits != state_size * 8
            || state->structure.alignment_bits != state_alignment * 8) {
            fail(std::string(label)
                 + " layout metadata does not match compiler ABI for '"
                 + record.type_name + "'");
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

class ConfigurationSchemaEmitter {
public:
    explicit ConfigurationSchemaEmitter(Module& module)
        : module_(module), context_(module.getContext()),
          pointer_type_(PointerType::getUnqual(context_)),
          size_type_(IntegerType::get(
              context_, module.getDataLayout().getPointerSizeInBits())),
          byte_type_(Type::getInt8Ty(context_)),
          int64_type_(Type::getInt64Ty(context_)),
          field_type_(StructType::get(
              pointer_type_, size_type_, size_type_, pointer_type_, pointer_type_)),
          enumerator_type_(StructType::get(
              pointer_type_, size_type_, int64_type_, int64_type_)),
          schema_type_(StructType::get(
              byte_type_, size_type_, size_type_, byte_type_, size_type_,
              pointer_type_, size_type_, pointer_type_, size_type_,
              pointer_type_, size_type_, pointer_type_, pointer_type_,
              pointer_type_, pointer_type_, pointer_type_, pointer_type_,
              pointer_type_, pointer_type_, pointer_type_))
    {
        auto const& layout = module_.getDataLayout();
        auto check = [&](Type* type, std::size_t expected, std::string_view name) {
            auto size = layout.getTypeAllocSize(type);
            if (size.isScalable() || size.getFixedValue() != expected)
                fail(std::string(name) + " ABI does not match provider declarations");
        };
        check(field_type_, sizeof(iv::details::ConfigurationExpressionFieldSchema),
            "configuration field schema");
        check(enumerator_type_,
            sizeof(iv::details::ConfigurationExpressionEnumeratorSchema),
            "configuration enumerator schema");
        check(schema_type_, sizeof(iv::details::ConfigurationExpressionTypeSchema),
            "configuration type schema");
    }

    Constant* emit(ConfigurationSchemaMetadata const& metadata)
    {
        auto const index = next_index_++;
        Constant* element = null_pointer();
        if (metadata.element_type) element = emit(*metadata.element_type);

        std::vector<Constant*> field_values;
        field_values.reserve(metadata.fields.size());
        for (auto const& field : metadata.fields) {
            if (!field.type || field.bit_offset % 8 != 0)
                fail("configuration aggregate field has an invalid byte layout");
            auto name = make_string(field.name,
                "schema." + std::to_string(index) + ".field_name");
            field_values.push_back(ConstantStruct::get(field_type_, {
                name.first,
                name.second,
                ConstantInt::get(size_type_, field.bit_offset / 8),
                emit(*field.type),
                function_pointer(field.assign_symbol, true),
            }));
        }
        Constant* fields = null_pointer();
        if (!field_values.empty()) {
            auto* array_type = ArrayType::get(field_type_, field_values.size());
            auto* storage = new GlobalVariable(
                module_, array_type, true, GlobalValue::PrivateLinkage,
                ConstantArray::get(array_type, field_values),
                "iv.configuration_schema." + std::to_string(index) + ".fields");
            storage->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
            fields = ConstantExpr::getPointerCast(storage, pointer_type_);
        }

        std::vector<Constant*> enumerator_values;
        enumerator_values.reserve(metadata.enumerators.size());
        for (auto const& enumerator : metadata.enumerators) {
            auto name = make_string(enumerator.name,
                "schema." + std::to_string(index) + ".enumerator_name");
            enumerator_values.push_back(ConstantStruct::get(enumerator_type_, {
                name.first,
                name.second,
                ConstantInt::getSigned(int64_type_, enumerator.signed_value),
                ConstantInt::get(int64_type_, enumerator.unsigned_value),
            }));
        }
        Constant* enumerators = null_pointer();
        if (!enumerator_values.empty()) {
            auto* array_type = ArrayType::get(
                enumerator_type_, enumerator_values.size());
            auto* storage = new GlobalVariable(
                module_, array_type, true, GlobalValue::PrivateLinkage,
                ConstantArray::get(array_type, enumerator_values),
                "iv.configuration_schema." + std::to_string(index)
                    + ".enumerators");
            storage->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
            enumerators = ConstantExpr::getPointerCast(storage, pointer_type_);
        }

        if (metadata.size_bits % 8 != 0 || metadata.alignment_bits % 8 != 0)
            fail("configuration expression type has a non-byte C++ layout");
        auto* initializer = ConstantStruct::get(schema_type_, {
            ConstantInt::get(byte_type_, kind(metadata.kind)),
            ConstantInt::get(size_type_, metadata.size_bits / 8),
            ConstantInt::get(size_type_, metadata.alignment_bits / 8),
            ConstantInt::get(byte_type_, metadata.integer_is_signed),
            ConstantInt::get(size_type_, metadata.integer_bit_width),
            fields,
            ConstantInt::get(size_type_, metadata.fields.size()),
            enumerators,
            ConstantInt::get(size_type_, metadata.enumerators.size()),
            element,
            ConstantInt::get(size_type_, metadata.fixed_element_count),
            function_pointer(metadata.construct_scalar_symbol, false),
            function_pointer(metadata.read_scalar_symbol, false),
            function_pointer(metadata.default_construct_symbol, false),
            function_pointer(metadata.destroy_symbol, true),
            function_pointer(metadata.element_assign_symbol, false),
            function_pointer(metadata.append_element_symbol, false),
            function_pointer(metadata.element_count_symbol, false),
            function_pointer(metadata.element_at_symbol, false),
            function_pointer(metadata.construct_node_address_symbol, false),
        });
        auto* global = new GlobalVariable(
            module_, schema_type_, true, GlobalValue::PrivateLinkage,
            initializer,
            "iv.configuration_schema." + std::to_string(index));
        global->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        return ConstantExpr::getPointerCast(global, pointer_type_);
    }

private:
    Module& module_;
    LLVMContext& context_;
    PointerType* pointer_type_;
    IntegerType* size_type_;
    IntegerType* byte_type_;
    IntegerType* int64_type_;
    StructType* field_type_;
    StructType* enumerator_type_;
    StructType* schema_type_;
    std::size_t next_index_ = 0;

    Constant* null_pointer() const
    {
        return ConstantPointerNull::get(pointer_type_);
    }

    std::pair<Constant*, Constant*> make_string(
        std::string_view value,
        std::string name)
    {
        auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        auto* storage = constant_bytes(module_, std::move(name), bytes, 1);
        return {
            ConstantExpr::getPointerCast(storage, pointer_type_),
            ConstantInt::get(size_type_, value.size()),
        };
    }

    Constant* function_pointer(std::string const& symbol, bool required)
    {
        if (symbol.empty()) {
            if (required) fail("configuration schema is missing a required callback");
            return null_pointer();
        }
        auto* value = module_.getNamedValue(symbol);
        auto* global = dyn_cast_or_null<GlobalValue>(value);
        if (!global) {
            fail("configuration schema callback '" + symbol
                 + "' was not emitted by the provider compiler");
        }
        return ConstantExpr::getPointerCast(global, pointer_type_);
    }

    static unsigned kind(std::string_view value)
    {
        using Kind = iv::details::ConfigurationExpressionTypeKind;
        if (value == "boolean") return static_cast<unsigned>(Kind::boolean);
        if (value == "signed_integer")
            return static_cast<unsigned>(Kind::signed_integer);
        if (value == "unsigned_integer")
            return static_cast<unsigned>(Kind::unsigned_integer);
        if (value == "floating_point")
            return static_cast<unsigned>(Kind::floating_point);
        if (value == "string") return static_cast<unsigned>(Kind::string);
        if (value == "converted_scalar")
            return static_cast<unsigned>(Kind::converted_scalar);
        if (value == "enumeration")
            return static_cast<unsigned>(Kind::enumeration);
        if (value == "aggregate") return static_cast<unsigned>(Kind::aggregate);
        if (value == "sequence") return static_cast<unsigned>(Kind::sequence);
        if (value == "node_reference")
            return static_cast<unsigned>(Kind::node_reference);
        if (value == "node_address")
            return static_cast<unsigned>(Kind::node_address);
        if (value == "unsupported")
            return static_cast<unsigned>(Kind::unsupported);
        fail("unknown configuration expression schema kind '" + std::string(value) + "'");
    }
};

void inject_configuration_type_identities(
    Module& module,
    CompilerMetadata const& metadata)
{
    auto& context = module.getContext();
    auto* pointer_type = PointerType::getUnqual(context);
    auto* size_type = IntegerType::get(
        context, module.getDataLayout().getPointerSizeInBits());
    // The C++ definition is emitted as a named LLVM struct.  This literal
    // struct is only the ABI shape against which that emitted definition is
    // checked; two structurally identical LLVM struct types are not pointer
    // equal when one is named.
    auto* identity_abi_type = StructType::get(
        pointer_type, size_type, pointer_type, size_type, pointer_type, size_type,
        pointer_type);
    auto const identity_size = module.getDataLayout().getTypeAllocSize(identity_abi_type);
    if (identity_size.isScalable()
        || identity_size.getFixedValue()
            != sizeof(iv::details::ConfigurationTypeIdentity)) {
        fail("configuration type-identity ABI does not match ConfigurationTypeIdentity");
    }

    std::unordered_set<std::string> metadata_symbols;
    metadata_symbols.reserve(metadata.configuration_types.size());
    for (auto const& metadata_type : metadata.configuration_types) {
        metadata_symbols.insert(metadata_type.symbol);
    }
    for (auto const& global : module.globals()) {
        if (global.getSection() != "iv_configuration_type_identities") continue;
        if (!metadata_symbols.contains(global.getName().str())) {
            fail("emitted configuration identity global '" + global.getName().str()
                 + "' has no compiler metadata");
        }
    }

    ConfigurationSchemaEmitter schema_emitter(module);
    for (std::size_t index = 0; index < metadata.configuration_types.size(); ++index) {
        auto const& metadata_type = metadata.configuration_types[index];
        auto* identity = module.getGlobalVariable(metadata_type.symbol, true);
        if (!identity) {
            identity = new GlobalVariable(
                module,
                identity_abi_type,
                false,
                GlobalValue::PrivateLinkage,
                UndefValue::get(identity_abi_type),
                metadata_type.symbol);
        } else if (identity->isDeclaration() || !identity->hasInitializer()) {
            fail("compiler metadata names incomplete configuration identity global '"
                 + metadata_type.symbol + "'");
        }
        auto* identity_type = dyn_cast<StructType>(identity->getValueType());
        if (!identity_type
            || identity_type->isOpaque()
            || !identity_type->isLayoutIdentical(identity_abi_type)) {
            fail("configuration identity global '" + metadata_type.symbol
                + "' has an incompatible LLVM type");
        }
        auto const make_string = [&](std::string_view field, StringRef suffix) {
            auto bytes = std::as_bytes(std::span(field.data(), field.size()));
            auto* storage = constant_bytes(
                module,
                "iv.configuration_type." + std::to_string(index) + "." + suffix.str(),
                bytes,
                1);
            return std::pair<Constant*, Constant*>{
                ConstantExpr::getPointerCast(storage, pointer_type),
                ConstantInt::get(size_type, field.size()),
            };
        };
        auto const nominal = make_string(metadata_type.nominal_id, "nominal");
        auto const fingerprint = make_string(
            metadata_type.definition_fingerprint, "fingerprint");
        auto const display = make_string(metadata_type.display_name, "display");
        auto* schema = schema_emitter.emit(metadata_type.expression_schema);
        identity->setInitializer(ConstantStruct::get(
            identity_type,
            {nominal.first, nominal.second,
             fingerprint.first, fingerprint.second,
             display.first, display.second,
             schema}));
        identity->setConstant(true);
    }
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
    for (auto* definition : definitions) {
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
    for (auto* definition : definitions) {
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

void inject_package_node_presentation_table(
    Module& module,
    std::span<GlobalVariable* const> presentations)
{
    auto* pointer_type = PointerType::getUnqual(module.getContext());
    if (presentations.empty()) {
        emit_view_accessor(
            module, "iv_package_node_presentations",
            ConstantPointerNull::get(pointer_type), 0);
        return;
    }

    auto* record_type = presentations.front()->getValueType();
    for (auto* presentation : presentations) {
        if (presentation->getValueType() != record_type) {
            fail("IV package node presentation records have inconsistent LLVM types");
        }
    }
    auto const record_size = module.getDataLayout().getTypeAllocSize(record_type);
    if (record_size.isScalable()
        || record_size.getFixedValue()
            != sizeof(iv::details::PackageNodePresentation)) {
        fail("IV package node presentation ABI does not match PackageNodePresentation");
    }

    std::vector<Constant*> values;
    values.reserve(presentations.size());
    for (auto* presentation : presentations) {
        values.push_back(presentation->getInitializer());
    }
    auto* array_type = ArrayType::get(record_type, values.size());
    auto* table = new GlobalVariable(
        module, array_type, true, GlobalValue::PrivateLinkage,
        ConstantArray::get(array_type, values), "iv.package_node_presentations");
    emit_view_accessor(
        module, "iv_package_node_presentations",
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

void inject_node_message_handler_table(
    Module& module,
    CompilerMetadata const& metadata)
{
    auto& context = module.getContext();
    auto* pointer_type = PointerType::getUnqual(context);
    auto* i64 = Type::getInt64Ty(context);
    auto* code_key_type = StructType::get(i64, i64);
    auto* record_type = StructType::get(
        code_key_type, pointer_type, pointer_type);
    auto const record_size = module.getDataLayout().getTypeAllocSize(record_type);
    if (record_size.isScalable()
        || record_size.getFixedValue() != sizeof(iv::NodeMessageHandlerData)) {
        fail("node-message-handler ABI does not match NodeMessageHandlerData");
    }
    auto* context_type = StructType::get(pointer_type, pointer_type);
    auto* wrapper_type = FunctionType::get(
        Type::getVoidTy(context),
        {pointer_type, pointer_type, pointer_type, pointer_type, pointer_type},
        false);

    std::vector<Constant*> records;
    for (auto const& handler : metadata.node_message_handlers) {
        if (handler.owner_kind
            != NodeMessageHandlerMetadata::OwnerKind::node) continue;
        if (!handler.node_code_key) {
            fail("node message handler has no NodeCodeKey");
        }
        auto* method = module.getFunction(handler.method_symbol);
        if (!method || method->isDeclaration()
            || !method->getReturnType()->isVoidTy()
            || method->arg_size() != 3) {
            fail("node message method has incompatible LLVM ABI: '"
                 + handler.method_symbol + "'");
        }
        auto* event_type = module.getGlobalVariable(
            handler.event_type_symbol, true);
        if (!event_type || event_type->isDeclaration()) {
            fail("node message handler has no event type identity: '"
                 + handler.event_type_symbol + "'");
        }

        auto* wrapper = Function::Create(
            wrapper_type,
            GlobalValue::PrivateLinkage,
            "iv.node_message_handler." + std::to_string(records.size()),
            module);
        auto argument = wrapper->arg_begin();
        auto* receiver = &*argument++;
        auto* operations = &*argument++;
        auto* address = &*argument++;
        ++argument; // ModuleMessageLocalRefs is unused for leaf-node handlers.
        auto* event = &*argument;
        auto* block = BasicBlock::Create(context, "entry", wrapper);
        IRBuilder<> builder(block);
        auto* message_context = builder.CreateAlloca(
            context_type, nullptr, "node.message.context");
        builder.CreateStore(
            operations,
            builder.CreateStructGEP(context_type, message_context, 0));
        builder.CreateStore(
            address,
            builder.CreateStructGEP(context_type, message_context, 1));
        auto* call = builder.CreateCall(
            method,
            {receiver, message_context, event});
        call->setCallingConv(method->getCallingConv());
        builder.CreateRetVoid();

        records.push_back(ConstantStruct::get(
            record_type,
            ConstantStruct::get(
                code_key_type,
                ConstantInt::get(i64, handler.node_code_key->low),
                ConstantInt::get(i64, handler.node_code_key->high)),
            ConstantExpr::getPointerCast(event_type, pointer_type),
            ConstantExpr::getPointerCast(wrapper, pointer_type)));
    }
    if (records.empty()) {
        emit_view_accessor(
            module,
            "iv_package_node_message_handlers",
            ConstantPointerNull::get(pointer_type),
            0);
        return;
    }
    auto* table_type = ArrayType::get(record_type, records.size());
    auto* table = new GlobalVariable(
        module,
        table_type,
        true,
        GlobalValue::PrivateLinkage,
        ConstantArray::get(table_type, records),
        "iv.package_node_message_handlers");
    emit_view_accessor(
        module,
        "iv_package_node_message_handlers",
        ConstantExpr::getPointerCast(table, pointer_type),
        records.size() * record_size.getFixedValue());
}

void inject_module_message_handler_table(
    Module& module,
    CompilerMetadata const& metadata)
{
    auto& context = module.getContext();
    auto* pointer_type = PointerType::getUnqual(context);
    auto const pointer_bits = module.getDataLayout().getPointerSizeInBits();
    auto* size_type = IntegerType::get(context, pointer_bits);
    auto* view_type = StructType::get(pointer_type, size_type);
    auto* record_type = StructType::get(
        view_type, pointer_type, pointer_type);
    auto const record_size = module.getDataLayout().getTypeAllocSize(record_type);
    if (record_size.isScalable()
        || record_size.getFixedValue() != sizeof(iv::ModuleMessageHandlerData)) {
        fail("module-message-handler ABI does not match ModuleMessageHandlerData");
    }
    auto* context_type = StructType::get(
        pointer_type, pointer_type, pointer_type);
    auto* wrapper_type = FunctionType::get(
        Type::getVoidTy(context),
        {pointer_type, pointer_type, pointer_type, pointer_type, pointer_type},
        false);

    std::vector<Constant*> records;
    for (auto const& handler : metadata.node_message_handlers) {
        if (handler.owner_kind
            != NodeMessageHandlerMetadata::OwnerKind::module) continue;
        if (handler.definition_id.empty()) {
            fail("module message handler has no definition ID");
        }
        auto* method = module.getFunction(handler.method_symbol);
        if (!method || method->isDeclaration()
            || !method->getReturnType()->isVoidTy()
            || method->arg_size() != 3) {
            fail("module message method has incompatible LLVM ABI: '"
                 + handler.method_symbol + "'");
        }
        auto* event_type = module.getGlobalVariable(
            handler.event_type_symbol, true);
        if (!event_type || event_type->isDeclaration()) {
            fail("module message handler has no event type identity: '"
                 + handler.event_type_symbol + "'");
        }

        auto* wrapper = Function::Create(
            wrapper_type,
            GlobalValue::PrivateLinkage,
            "iv.module_message_handler." + std::to_string(records.size()),
            module);
        auto argument = wrapper->arg_begin();
        auto* receiver = &*argument++;
        auto* operations = &*argument++;
        auto* address = &*argument++;
        auto* local_refs = &*argument++;
        auto* event = &*argument;
        auto* block = BasicBlock::Create(context, "entry", wrapper);
        IRBuilder<> builder(block);
        auto* message_context = builder.CreateAlloca(
            context_type, nullptr, "module.message.context");
        builder.CreateStore(
            operations,
            builder.CreateStructGEP(context_type, message_context, 0));
        builder.CreateStore(
            address,
            builder.CreateStructGEP(context_type, message_context, 1));
        builder.CreateStore(
            local_refs,
            builder.CreateStructGEP(context_type, message_context, 2));
        auto* call = builder.CreateCall(
            method,
            {receiver, message_context, event});
        call->setCallingConv(method->getCallingConv());
        builder.CreateRetVoid();

        records.push_back(ConstantStruct::get(
            record_type,
            string_view_constant(
                module,
                view_type,
                "iv.module_message_handler.definition_id."
                    + std::to_string(records.size()),
                handler.definition_id),
            ConstantExpr::getPointerCast(event_type, pointer_type),
            ConstantExpr::getPointerCast(wrapper, pointer_type)));
    }
    if (records.empty()) {
        emit_view_accessor(
            module,
            "iv_package_module_message_handlers",
            ConstantPointerNull::get(pointer_type),
            0);
        return;
    }
    auto* table_type = ArrayType::get(record_type, records.size());
    auto* table = new GlobalVariable(
        module,
        table_type,
        true,
        GlobalValue::PrivateLinkage,
        ConstantArray::get(table_type, records),
        "iv.package_module_message_handlers");
    emit_view_accessor(
        module,
        "iv_package_module_message_handlers",
        ConstantExpr::getPointerCast(table, pointer_type),
        records.size() * record_size.getFixedValue());
}

void inject_package_configuration_metadata(
    Module& module,
    CompilerMetadata const& metadata,
    std::span<std::pair<iv::NodeCodeKey, iv::NodeStateStructure> const> state_structures,
    std::span<std::pair<iv::NodeCodeKey, iv::NodeStateStructure> const> background_state_structures,
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
    for (std::size_t index = 0; index < retained_globals.size(); ++index) {
        globals.push_back(ConstantStruct::get(
            retained_global_type,
            ConstantExpr::getPointerCast(retained_globals[index].global, pointer_type),
            ConstantInt::get(size_type, retained_globals[index].size),
            ConstantInt::get(size_type, index)));
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
        view_type,
        view_type,
        view_type,
        size_type,
        size_type,
        size_type,
        size_type,
        i8);
    auto const state_field_size = module.getDataLayout().getTypeAllocSize(state_field_type);
    if (state_field_size.isScalable()
        || state_field_size.getFixedValue() != sizeof(iv::NodeStateFieldData)) {
        fail("node-state field ABI does not match NodeStateFieldData");
    }
    auto* state_structure_type = StructType::get(
        code_key_type,
        view_type,
        view_type,
        view_type,
        size_type,
        size_type,
        view_type);
    auto const state_structure_size = module.getDataLayout().getTypeAllocSize(state_structure_type);
    if (state_structure_size.isScalable()
        || state_structure_size.getFixedValue() != sizeof(iv::NodeStateStructureData)) {
        fail("node-state structure ABI does not match NodeStateStructureData");
    }

    auto emit_state_structures = [&](StringRef accessor_name,
                                     StringRef global_name,
                                     StringRef value_prefix,
                                     std::span<std::pair<iv::NodeCodeKey, iv::NodeStateStructure> const> values) {
        std::vector<Constant*> structures;
        structures.reserve(values.size());
        for (std::size_t structure_index = 0;
             structure_index < values.size(); ++structure_index) {
            auto const& [key, state] = values[structure_index];
            if (!state.type_identity.valid()) {
                fail("node-state structure has incomplete type definition identity");
            }
            std::vector<Constant*> fields;
            fields.reserve(state.fields.size());
            for (std::size_t field_index = 0; field_index < state.fields.size(); ++field_index) {
                auto const& field = state.fields[field_index];
                auto const prefix = value_prefix.str() + "." + std::to_string(structure_index)
                    + ".field." + std::to_string(field_index);
                fields.push_back(ConstantStruct::get(
                    state_field_type,
                    string_view_constant(module, view_type, prefix + ".name", field.name),
                    string_view_constant(
                        module,
                        view_type,
                        prefix + ".nominal",
                        field.nominal_id),
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
                    value_prefix.str() + "_fields." + std::to_string(structure_index));
                fields_view = ConstantStruct::get(
                    view_type,
                    ConstantExpr::getPointerCast(fields_global, pointer_type),
                    ConstantInt::get(
                        size_type, fields.size() * state_field_size.getFixedValue()));
            }
            auto const identity_prefix = value_prefix.str() + "."
                + std::to_string(structure_index) + ".identity";
            structures.push_back(ConstantStruct::get(
                state_structure_type,
                code_key(key),
                string_view_constant(
                    module, view_type, identity_prefix + ".nominal",
                    state.type_identity.nominal_id),
                string_view_constant(
                    module, view_type, identity_prefix + ".fingerprint",
                    state.type_identity.definition_fingerprint),
                string_view_constant(
                    module, view_type, identity_prefix + ".display",
                    state.type_identity.display_name),
                ConstantInt::get(size_type, state.size_bits),
                ConstantInt::get(size_type, state.alignment_bits),
                fields_view));
        }
        if (structures.empty()) {
            emit_view_accessor(
                module, accessor_name,
                ConstantPointerNull::get(pointer_type), 0);
            return;
        }
        auto* array_type = ArrayType::get(state_structure_type, structures.size());
        auto* table = new GlobalVariable(
            module, array_type, true, GlobalValue::PrivateLinkage,
            ConstantArray::get(array_type, structures), global_name);
        emit_view_accessor(
            module, accessor_name,
            ConstantExpr::getPointerCast(table, pointer_type),
            structures.size() * state_structure_size.getFixedValue());
    };

    emit_state_structures(
        "iv_package_node_state_structures",
        "iv.package_node_state_structures",
        "iv.package_state",
        state_structures);
    emit_state_structures(
        "iv_package_node_background_state_structures",
        "iv.package_node_background_state_structures",
        "iv.package_background_state",
        background_state_structures);

}

// Checks that the package defines every entry point its configuration JIT
// looks up, then removes code that no external root reaches. Nothing is pinned
// in llvm.used. The finalized LLVM has two consumers: the configuration JIT,
// which looks up these external entry points, and reduce_to_node_code(), which
// reaches each callback through its node record. A pin would keep an
// out-of-line copy of every function the pre-link inlines. JITLink drops such
// unreferenced local copies, and each dropped .eh_frame record leaves zero
// padding that ends libgcc's frame search early, so an exception through any
// later function terminates (Front 16, 2026-11-07).
void remove_unreachable_package_code(Module& module)
{
    static constexpr std::array<StringRef, 9> entry_points{
        "iv_package_abi_version",
        "iv_package_definitions",
        "iv_package_node_presentations",
        "iv_package_node_config_pointer_fields",
        "iv_package_retained_globals",
        "iv_package_node_message_handlers",
        "iv_package_module_message_handlers",
        "iv_package_node_state_structures",
        "iv_package_node_background_state_structures",
    };
    for (auto const name : entry_points) {
        auto* function = module.getFunction(name);
        if (!function || function->isDeclaration()) {
            fail("finalized IV package does not define '" + name.str() + "'");
        }
    }
    legacy::PassManager passes;
    passes.add(createGlobalDCEPass());
    passes.run(module);
}

// Package-revision work that no graph edit changes belongs here, once per
// package build, not in every GraphJit compilation. LLVM's ThinLTO pre-link
// simplification pipeline (O2) inlines and simplifies within the whole linked
// package; GlobalDCE then removes every copy that inlining left unreferenced
// (see remove_unreachable_package_code()). Node callbacks stay functions with
// bodies because their node records take their addresses. The finalized LLVM
// is what the package's own ORC JIT loads for configuration; node code is
// optimized further from it below.
void prelink_optimize_package(Module& module)
{
    // Clang's O0 barrier pair, from O0 LLVM inputs that custom package CMake
    // may add. An explicit noinline without optnone is the author's and stays.
    for (auto& function : module) {
        if (function.isDeclaration()
            || !function.hasFnAttribute(Attribute::OptimizeNone)) {
            continue;
        }
        function.removeFnAttr(Attribute::OptimizeNone);
        function.removeFnAttr(Attribute::NoInline);
    }

    InitializeNativeTarget();
    auto triple = module.getTargetTriple();
    if (triple.empty()) triple = Triple(sys::getProcessTriple());
    std::string error;
    auto const* target = TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        fail("no LLVM target for IV package triple '" + triple.str() + "': " + error);
    }
    // No CPU or features: each function's own target attributes select its
    // subtarget for the cost model.
    std::unique_ptr<TargetMachine> target_machine(target->createTargetMachine(
        triple, "", "", TargetOptions(), std::nullopt));
    if (!target_machine) {
        fail("cannot create the LLVM target machine for IV package pre-link optimization");
    }

    PassBuilder pass_builder(target_machine.get());
    LoopAnalysisManager loops;
    FunctionAnalysisManager functions;
    CGSCCAnalysisManager cgscc;
    ModuleAnalysisManager modules;
    pass_builder.registerModuleAnalyses(modules);
    pass_builder.registerCGSCCAnalyses(cgscc);
    pass_builder.registerFunctionAnalyses(functions);
    pass_builder.registerLoopAnalyses(loops);
    pass_builder.crossRegisterProxies(loops, functions, cgscc, modules);
    auto pipeline = pass_builder.buildThinLTOPreLinkDefaultPipeline(OptimizationLevel::O2);
    pipeline.addPass(GlobalDCEPass());
    pipeline.run(module, modules);
}

void verify_finalized_package(Module const& module)
{
    std::string diagnostics;
    raw_string_ostream stream(diagnostics);
    if (!verifyModule(module, &stream)) return;
    stream.flush();
    fail("finalized IV package LLVM is invalid:\n" + diagnostics);
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

// Node code: the native object beside the finalized package LLVM that project
// code calls. See NodeCodeRecordData in module/abi.h.
std::filesystem::path node_code_object_path(std::filesystem::path const& bitcode_path)
{
    return std::filesystem::path(bitcode_path.string() + ".node-code.o");
}

// Node code is compiled for the CPU it runs on. ModuleLoader puts the same host
// CPU and features in the package artifact signature, so a cached object is
// never loaded on another CPU.
struct NodeCodeTarget {
    std::string cpu;
    std::string features;
};

NodeCodeTarget host_node_code_target()
{
    SubtargetFeatures features;
    for (auto const& feature : sys::getHostCPUFeatures()) {
        features.AddFeature(feature.getKey(), feature.getValue());
    }
    return {
        .cpu = sys::getHostCPUName().str(),
        .features = features.getString(),
    };
}

std::unique_ptr<TargetMachine> create_node_code_target_machine(
    Module const& module,
    NodeCodeTarget const& host)
{
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    // Object emission assembles inline asm in package code.
    InitializeNativeTargetAsmParser();
    auto triple = module.getTargetTriple();
    if (triple.empty()) triple = Triple(sys::getProcessTriple());
    std::string error;
    auto const* target = TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        fail("no LLVM target for IV package node code '" + triple.str() + "': " + error);
    }
    // Position-independent code with the small code model is what ORC links
    // anywhere in the address space.
    std::unique_ptr<TargetMachine> target_machine(target->createTargetMachine(
        triple,
        host.cpu,
        host.features,
        TargetOptions(),
        Reloc::PIC_,
        CodeModel::Small,
        CodeGenOptLevel::Aggressive));
    if (!target_machine) {
        fail("cannot create the LLVM target machine for IV package node code");
    }
    if (target_machine->createDataLayout() != module.getDataLayout()) {
        fail("IV package LLVM data layout does not match the host node-code target");
    }
    return target_machine;
}

// The data pointer a finalizer view accessor (emit_view_accessor) returns, or
// null when the accessor returns an empty view.
Constant* view_accessor_data(Module& module, StringRef name)
{
    auto* accessor = module.getFunction(name);
    if (!accessor || accessor->isDeclaration()) {
        fail("finalized IV package does not define '" + name.str() + "'");
    }
    for (auto& block : *accessor) {
        auto* ret = dyn_cast_or_null<ReturnInst>(block.getTerminator());
        if (!ret) continue;
        auto* view = dyn_cast_or_null<ConstantStruct>(ret->getReturnValue());
        if (!view || view->getNumOperands() != 2) break;
        auto* data = view->getOperand(0);
        return isa<ConstantPointerNull>(data) ? nullptr : data;
    }
    fail("finalized IV package accessor '" + name.str() + "' does not return a constant view");
}

std::size_t node_code_size(Value const* value, std::string_view field)
{
    auto const* integer = dyn_cast<ConstantInt>(value);
    if (!integer || integer->getValue().getActiveBits() > 64
        || integer->getZExtValue() > std::numeric_limits<std::size_t>::max()) {
        fail("malformed IV package " + std::string(field));
    }
    return static_cast<std::size_t>(integer->getZExtValue());
}

// Reduces the finalized package, after its LLVM has been written, to node code:
// every node record's runtime/compiler callbacks and every retained global,
// reachable only from the two node-code tables. Everything else becomes
// internal and unreferenced, so optimization removes the configuration code,
// the package tables, and the static constructors. Node runtime operations
// reference no mutable package global (reject_node_runtime_mutable_globals),
// so node code needs no constructors.
void reduce_to_node_code(Module& module, NodeCodeTarget const& host)
{
    auto& context = module.getContext();
    auto const& layout = module.getDataLayout();
    auto* size_type = IntegerType::get(context, layout.getPointerSizeInBits());
    auto* pointer_type = PointerType::getUnqual(context);

    auto* i64 = Type::getInt64Ty(context);
    auto* key_type = StructType::get(i64, i64);
    auto* operations_type = StructType::get(
        context, SmallVector<Type*, 13>(13, pointer_type));
    auto* record_type = StructType::get(key_type, operations_type);
    auto const record_size = layout.getTypeAllocSize(record_type);
    if (record_size.isScalable()
        || record_size.getFixedValue() != sizeof(iv::NodeCodeRecordData)) {
        fail("node-code record ABI does not match NodeCodeRecordData");
    }

    struct Record {
        iv::NodeCodeKey key{};
        ConstantStruct* operations = nullptr;
    };
    std::vector<Record> records;
    for (auto& global : module.globals()) {
        auto const section = global.getSection();
        if (section != "iv_node_types" && !section.ends_with("__iv_node_types")) continue;
        auto* record = global.hasInitializer()
            ? dyn_cast<ConstantStruct>(global.getInitializer())
            : nullptr;
        if (!record || record->getNumOperands() != 11) fail("malformed iv_node_types record");
        auto* key = dyn_cast<ConstantStruct>(record->getOperand(0));
        auto* operations = dyn_cast<ConstantStruct>(record->getOperand(1));
        if (!key || key->getNumOperands() != 2 || !operations
            || operations->getNumOperands() != 13) {
            fail("malformed iv_node_types record ABI");
        }
        // Operand 0 is declare_node, which stays configuration-only.
        for (unsigned index = 0; index < operations->getNumOperands(); ++index) {
            auto* callback = operations->getOperand(index);
            if (!callback->getType()->isPointerTy()) {
                fail("malformed iv_node_types operations ABI");
            }
            if (index == 0 || isa<ConstantPointerNull>(callback)) continue;
            auto* function = dyn_cast<Function>(callback->stripPointerCasts());
            if (!function || function->isDeclaration()) {
                fail("node compiler record callback has no definition in the finalized package");
            }
        }
        records.push_back({
            .key = {
                .low = constant_u64(key->getOperand(0)),
                .high = constant_u64(key->getOperand(1)),
            },
            .operations = operations,
        });
    }
    // Sorted so that GraphJit finds a record by binary search.
    std::ranges::sort(records, {}, &Record::key);
    if (std::ranges::adjacent_find(records, {}, &Record::key) != records.end()) {
        fail("duplicate NodeCodeKey in compiler records");
    }

    std::vector<Constant*> record_entries;
    record_entries.reserve(records.size());
    for (auto const& record : records) {
        std::vector<Constant*> operations;
        operations.push_back(ConstantPointerNull::get(pointer_type));
        for (unsigned index = 1; index < record.operations->getNumOperands(); ++index) {
            operations.push_back(record.operations->getOperand(index));
        }
        record_entries.push_back(ConstantStruct::get(
            record_type,
            ConstantStruct::get(
                key_type,
                ConstantInt::get(i64, record.key.low),
                ConstantInt::get(i64, record.key.high)),
            ConstantStruct::get(operations_type, operations)));
    }

    auto* retained_type = StructType::get(pointer_type, size_type, size_type);
    auto const retained_size = layout.getTypeAllocSize(retained_type);
    if (retained_size.isScalable()
        || retained_size.getFixedValue() != sizeof(iv::NodeCodeRetainedGlobalData)) {
        fail("node-code retained-global ABI does not match NodeCodeRetainedGlobalData");
    }
    std::vector<Constant*> retained_entries;
    if (auto* data = view_accessor_data(module, "iv_package_retained_globals")) {
        auto* table = dyn_cast<GlobalVariable>(data->stripPointerCasts());
        auto* array = table && table->hasInitializer()
            ? dyn_cast<ConstantArray>(table->getInitializer())
            : nullptr;
        if (!array) fail("malformed IV package retained-global table");
        retained_entries.assign(array->getNumOperands(), nullptr);
        for (unsigned position = 0; position < array->getNumOperands(); ++position) {
            auto* entry = dyn_cast<ConstantStruct>(array->getOperand(position));
            if (!entry || entry->getNumOperands() != 3) {
                fail("malformed IV package retained-global record");
            }
            auto const size = node_code_size(entry->getOperand(1), "retained-global size");
            auto const index = node_code_size(entry->getOperand(2), "retained-global index");
            if (index >= retained_entries.size() || retained_entries[index]) {
                fail("invalid IV package retained-global index");
            }
            auto* global = dyn_cast<GlobalVariable>(entry->getOperand(0)->stripPointerCasts());
            if (!global || !global->isConstant() || !global->hasInitializer()
                || global->isThreadLocal() || global->getAddressSpace() != 0) {
                retained_entries[index] = ConstantStruct::get(
                    retained_type,
                    ConstantPointerNull::get(pointer_type),
                    ConstantInt::get(size_type, 0),
                    ConstantInt::get(size_type, 1));
                continue;
            }
            // An explicit alignment makes the reported one exact.
            if (!global->getAlign()) global->setAlignment(layout.getPreferredAlign(global));
            retained_entries[index] = ConstantStruct::get(
                retained_type,
                ConstantExpr::getPointerCast(global, pointer_type),
                ConstantInt::get(size_type, size),
                ConstantInt::get(size_type, global->getAlign()->value()));
        }
    }

    // The package's retention lists, static constructors and destructors, and
    // annotations are configuration roots, not node code.
    for (auto& global : make_early_inc_range(module.globals())) {
        if (global.hasAppendingLinkage()) global.eraseFromParent();
    }

    auto emit_table = [&](StringRef accessor, StringRef table_name, Type* type,
                          std::vector<Constant*> const& entries) -> Function* {
        if (entries.empty()) {
            return emit_view_accessor(
                module, accessor, ConstantPointerNull::get(pointer_type), 0);
        }
        auto* array_type = ArrayType::get(type, entries.size());
        auto* table = new GlobalVariable(
            module, array_type, true, GlobalValue::PrivateLinkage,
            ConstantArray::get(array_type, entries), table_name);
        return emit_view_accessor(
            module, accessor,
            ConstantExpr::getPointerCast(table, pointer_type),
            layout.getTypeAllocSize(array_type).getFixedValue());
    };
    auto* records_accessor = emit_table(
        "iv_package_node_code_records", "iv.node_code_records",
        record_type, record_entries);
    auto* retained_accessor = emit_table(
        "iv_package_node_code_retained_globals", "iv.node_code_retained_globals",
        retained_type, retained_entries);
    if (records_accessor->getName() != "iv_package_node_code_records"
        || retained_accessor->getName() != "iv_package_node_code_retained_globals") {
        fail("IV package defines a reserved node-code symbol");
    }

    auto internalize = [&](GlobalValue& value) {
        if (value.isDeclaration() || value.hasLocalLinkage()
            || value.hasAvailableExternallyLinkage()
            || &value == records_accessor || &value == retained_accessor) {
            return;
        }
        value.setLinkage(GlobalValue::InternalLinkage);
        value.setVisibility(GlobalValue::DefaultVisibility);
        value.setDLLStorageClass(GlobalValue::DefaultStorageClass);
        if (auto* object = dyn_cast<GlobalObject>(&value)) object->setComdat(nullptr);
    };
    for (auto& function : module) internalize(function);
    for (auto& global : module.globals()) internalize(global);
    for (auto& alias : module.aliases()) internalize(alias);
    for (auto& ifunc : module.ifuncs()) internalize(ifunc);

    // Clang recorded the generic baseline CPU on each function. Node code is
    // for the host; a function's own explicit features still apply after it.
    for (auto& function : module) {
        if (function.isDeclaration()) continue;
        auto features = host.features;
        if (auto const existing = function.getFnAttribute("target-features");
            existing.isValid() && !existing.getValueAsString().empty()) {
            features += ',';
            features += existing.getValueAsString();
        }
        function.addFnAttr("target-cpu", host.cpu);
        function.addFnAttr("target-features", features);
        function.removeFnAttr("tune-cpu");
    }
}

void verify_node_code(Module const& module)
{
    std::string diagnostics;
    raw_string_ostream stream(diagnostics);
    if (!verifyModule(module, &stream)) return;
    stream.flush();
    fail("IV package node code LLVM is invalid:\n" + diagnostics);
}

// Node code runs on the audio thread and is compiled once per package build, so
// it gets the complete O3 pipeline with Clang's O3 tuning (loop and SLP
// vectorization, interleaving, and unrolling). GlobalDCE first removes what
// reduce_to_node_code() left unreferenced.
void optimize_node_code(Module& module, TargetMachine& target_machine)
{
    PipelineTuningOptions tuning;
    tuning.LoopInterleaving = true;
    tuning.LoopVectorization = true;
    tuning.SLPVectorization = true;
    tuning.LoopUnrolling = true;
    PassBuilder pass_builder(&target_machine, tuning);
    LoopAnalysisManager loops;
    FunctionAnalysisManager functions;
    CGSCCAnalysisManager cgscc;
    ModuleAnalysisManager modules;
    pass_builder.registerModuleAnalyses(modules);
    pass_builder.registerCGSCCAnalyses(cgscc);
    pass_builder.registerFunctionAnalyses(functions);
    pass_builder.registerLoopAnalyses(loops);
    pass_builder.crossRegisterProxies(loops, functions, cgscc, modules);
    ModulePassManager pipeline;
    pipeline.addPass(GlobalDCEPass());
    pipeline.addPass(pass_builder.buildPerModuleDefaultPipeline(OptimizationLevel::O3));
    pipeline.run(module, modules);
}

void write_node_code_object(
    Module& module,
    TargetMachine& target_machine,
    std::filesystem::path const& path)
{
    auto temporary = path;
    temporary += ".tmp";
    {
        std::error_code ec;
        raw_fd_ostream output(temporary.string(), ec, sys::fs::OF_None);
        if (ec) {
            fail("cannot create IV package node code '" + temporary.string()
                + "': " + ec.message());
        }
        legacy::PassManager passes;
        if (target_machine.addPassesToEmitFile(
                passes, output, nullptr, CodeGenFileType::ObjectFile)) {
            fail("the LLVM target cannot emit IV package node code objects");
        }
        passes.run(module);
        output.flush();
        if (output.has_error()) {
            fail("cannot finish IV package node code '" + temporary.string() + "'");
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            fail("cannot publish IV package node code '" + path.string()
                + "': " + ec.message());
        }
    }
}

json::Object node_event_value_schema_json(
    ConfigurationSchemaMetadata const& schema)
{
    json::Object result;
    auto const set_kind = [&](std::string_view kind) {
        result["kind"] = std::string(kind);
    };

    if (schema.kind == "boolean") {
        set_kind("boolean");
    } else if (schema.kind == "signed_integer") {
        set_kind("signedInteger");
        result["bitWidth"] = static_cast<std::int64_t>(schema.integer_bit_width);
    } else if (schema.kind == "unsigned_integer") {
        set_kind("unsignedInteger");
        result["bitWidth"] = static_cast<std::int64_t>(schema.integer_bit_width);
    } else if (schema.kind == "floating_point") {
        set_kind("floatingPoint");
        result["bitWidth"] = static_cast<std::int64_t>(schema.size_bits);
    } else if (schema.kind == "string") {
        set_kind("string");
    } else if (schema.kind == "converted_scalar") {
        set_kind("convertedScalar");
        // The configuration-expression boundary accepts each JSON scalar form
        // and lets the provider constructor perform its exact validation.
        json::Array json_kinds;
        json_kinds.push_back("boolean");
        json_kinds.push_back("number");
        json_kinds.push_back("string");
        result["jsonKinds"] = std::move(json_kinds);
        result["providerValidated"] = true;
    } else if (schema.kind == "enumeration") {
        set_kind("enumeration");
        result["bitWidth"] = static_cast<std::int64_t>(schema.integer_bit_width);
        result["signed"] = schema.integer_is_signed;
        json::Array enumerators;
        for (auto const& enumerator : schema.enumerators) {
            enumerators.push_back(json::Object{
                {"name", enumerator.name},
                {"value", schema.integer_is_signed
                    ? std::to_string(enumerator.signed_value)
                    : std::to_string(enumerator.unsigned_value)},
            });
        }
        result["enumerators"] = std::move(enumerators);
    } else if (schema.kind == "aggregate") {
        set_kind("aggregate");
        json::Array fields;
        for (auto const& field : schema.fields) {
            if (!field.type) {
                fail("event aggregate field '" + field.name + "' has no type schema");
            }
            fields.push_back(json::Object{
                {"name", field.name},
                {"value", node_event_value_schema_json(*field.type)},
            });
        }
        result["fields"] = std::move(fields);
    } else if (schema.kind == "sequence") {
        set_kind("sequence");
        if (!schema.element_type) {
            fail("event sequence has no element type schema");
        }
        result["element"] = node_event_value_schema_json(*schema.element_type);
        if (schema.fixed_element_count != 0) {
            result["fixedElementCount"] = static_cast<std::int64_t>(
                schema.fixed_element_count);
        }
    } else if (schema.kind == "node_address") {
        set_kind("nodeAddress");
    } else if (schema.kind == "node_reference") {
        set_kind("unsupported");
        result["reason"] =
            "project NodeRef values cannot cross nodeMessages.dispatch";
    } else {
        set_kind("unsupported");
        result["reason"] = "provider type has no semantic event JSON representation";
    }
    return result;
}

std::filesystem::path node_event_protocol_path(
    std::filesystem::path const& bitcode_path)
{
    return bitcode_path.string() + ".node-events.json";
}

void write_node_event_protocol(
    CompilerMetadata const& metadata,
    std::filesystem::path const& bitcode_path)
{
    std::unordered_set<std::string> event_symbols;
    for (auto const& handler : metadata.node_message_handlers) {
        event_symbols.insert(handler.event_type_symbol);
    }

    auto const event_type = [&](std::string const& symbol)
        -> ConfigurationTypeMetadata const& {
        auto const found = std::ranges::find_if(
            metadata.configuration_types,
            [&](ConfigurationTypeMetadata const& type) {
                return type.symbol == symbol;
            });
        if (found == metadata.configuration_types.end()) {
            fail("node-message event type '" + symbol
                + "' has no configuration type metadata");
        }
        return *found;
    };

    std::vector<ConfigurationTypeMetadata const*> events;
    events.reserve(event_symbols.size());
    for (auto const& symbol : event_symbols) {
        events.push_back(std::addressof(event_type(symbol)));
    }
    std::ranges::sort(events, [](auto const* lhs, auto const* rhs) {
        return std::tie(lhs->nominal_id, lhs->definition_fingerprint)
            < std::tie(rhs->nominal_id, rhs->definition_fingerprint);
    });

    json::Array event_json;
    for (std::size_t index = 0; index < events.size(); ++index) {
        auto const& event = *events[index];
        if (index != 0) {
            auto const& previous = *events[index - 1];
            if (event.nominal_id == previous.nominal_id
                && event.definition_fingerprint == previous.definition_fingerprint) {
                if (!same_configuration_schema(
                        event.expression_schema, previous.expression_schema)) {
                    fail("semantic event identity '" + event.nominal_id
                        + "' has conflicting payload schemas");
                }
                continue;
            }
        }
        event_json.push_back(json::Object{
            {"nominalId", event.nominal_id},
            {"definitionFingerprint", event.definition_fingerprint},
            {"displayName", event.display_name},
            {"payload", node_event_value_schema_json(event.expression_schema)},
        });
    }

    using EventIdentity = std::pair<std::string, std::string>;
    std::map<std::string, std::vector<EventIdentity>> definition_events;
    for (auto const& handler : metadata.node_message_handlers) {
        std::string definition_id;
        if (handler.owner_kind == NodeMessageHandlerMetadata::OwnerKind::module) {
            definition_id = handler.definition_id;
        } else if (handler.node_code_key) {
            auto const definition = std::ranges::find_if(
                metadata.package_definitions,
                [&](PackageDefinitionMetadata const& value) {
                    return value.kind == "node"
                        && value.node_code_key == handler.node_code_key;
                });
            if (definition != metadata.package_definitions.end()) {
                definition_id = definition->id;
            }
        }
        // An unregistered leaf type has no stable public definition ID. Its
        // concrete support remains available from the runtime address catalog.
        if (definition_id.empty()) continue;
        auto const& event = event_type(handler.event_type_symbol);
        definition_events[definition_id].emplace_back(
            event.nominal_id, event.definition_fingerprint);
    }
    json::Array definition_json;
    for (auto& [definition_id, supported] : definition_events) {
        std::ranges::sort(supported);
        auto const duplicate = std::ranges::unique(supported);
        supported.erase(duplicate.begin(), duplicate.end());
        json::Array supported_json;
        for (auto const& [nominal_id, fingerprint] : supported) {
            supported_json.push_back(json::Object{
                {"nominalId", nominal_id},
                {"definitionFingerprint", fingerprint},
            });
        }
        definition_json.push_back(json::Object{
            {"definitionId", definition_id},
            {"supportedEventTypes", std::move(supported_json)},
        });
    }

    auto const path = node_event_protocol_path(bitcode_path);
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail("cannot create semantic event protocol '" + temporary.string() + "'");
    }
    output << formatv("{0:2}", json::Value(json::Object{
        {"formatVersion", std::int64_t{0}},
        {"eventTypes", std::move(event_json)},
        {"definitions", std::move(definition_json)},
    })).str() << '\n';
    output.flush();
    if (!output) {
        fail("cannot finish semantic event protocol '" + temporary.string() + "'");
    }
    output.close();
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) {
            fail("cannot publish semantic event protocol '" + path.string()
                + "': " + error.message());
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
    auto state_structures = bind_state_metadata(
        node_records, metadata.states, false);
    auto background_state_structures = bind_state_metadata(
        node_records, metadata.background_states, true);
    require_node_config_metadata(node_records, metadata);
    auto definitions = package_definition_globals(package);
    validate_package_definitions(definitions, metadata);
    auto presentations = package_node_presentation_globals(package);
    validate_package_node_presentations(presentations, definitions);
    auto retained_globals = collect_retained_globals(package, definitions);
    timings.finish_stage("package_metadata_validate", stage_started_at);

    stage_started_at = timings.start_stage();
    inject_package_abi_version(package);
    inject_configuration_type_identities(package, metadata);
    inject_node_message_handler_table(package, metadata);
    inject_module_message_handler_table(package, metadata);
    inject_package_definition_table(package, definitions);
    inject_package_node_presentation_table(package, presentations);
    inject_package_configuration_metadata(
        package,
        metadata,
        state_structures,
        background_state_structures,
        retained_globals);
    remove_unreachable_package_code(package);
    verify_finalized_package(package);
    timings.finish_stage("package_metadata_inject", stage_started_at);

    stage_started_at = timings.start_stage();
    prelink_optimize_package(package);
    verify_finalized_package(package);
    timings.finish_stage("package_prelink_optimize", stage_started_at);

    stage_started_at = timings.start_stage();
    write_package_bitcode(package, options.output);
    timings.finish_stage("package_bitcode_write", stage_started_at);

    stage_started_at = timings.start_stage();
    write_node_event_protocol(metadata, options.output);
    timings.finish_stage("node_event_protocol_write", stage_started_at);

    // The finalized LLVM is written; the module is now reduced in place to
    // node code.
    stage_started_at = timings.start_stage();
    auto const node_code_host = host_node_code_target();
    auto node_code_target = create_node_code_target_machine(package, node_code_host);
    reduce_to_node_code(package, node_code_host);
    verify_node_code(package);
    timings.finish_stage("node_code_reduce", stage_started_at);

    stage_started_at = timings.start_stage();
    optimize_node_code(package, *node_code_target);
    verify_node_code(package);
    timings.finish_stage("node_code_optimize", stage_started_at);

    stage_started_at = timings.start_stage();
    write_node_code_object(package, *node_code_target, node_code_object_path(options.output));
    timings.finish_stage("node_code_codegen", stage_started_at);

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
