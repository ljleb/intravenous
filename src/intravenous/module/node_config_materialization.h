#pragma once

#include <intravenous/module/abi.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv::details {

inline bool is_valid_node_config_alignment(std::size_t alignment)
{
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

struct OwnedNodeConfigBytes {
    void* data = nullptr;
    std::size_t alignment = 1;
    std::vector<std::string> strings;

    OwnedNodeConfigBytes(std::size_t size, std::size_t alignment_)
        : alignment(alignment_)
    {
        data = ::operator new(size, std::align_val_t(alignment));
    }

    ~OwnedNodeConfigBytes()
    {
        ::operator delete(data, std::align_val_t(alignment));
    }

    OwnedNodeConfigBytes(OwnedNodeConfigBytes const&) = delete;
    OwnedNodeConfigBytes& operator=(OwnedNodeConfigBytes const&) = delete;
};

struct MaterializedNodeConfigs {
    std::vector<ModuleNodeConfigRecord> records;
    std::vector<std::shared_ptr<void const>> storage;
};

inline MaterializedNodeConfigs materialize_node_configs(
    std::span<ModuleNodeConfigRecord const> configs,
    std::span<ModuleNodeConfigStringRelocationRecord const> relocations)
{
    std::vector<std::size_t> string_counts(configs.size());
    std::vector<std::unordered_set<std::size_t>> string_offsets(configs.size());
    for (auto const& relocation : relocations) {
        if (relocation.config_ordinal >= configs.size())
            throw std::runtime_error("module string relocation has invalid config ordinal");
        auto const& config = configs[relocation.config_ordinal];
        if (relocation.byte_offset > config.size
            || config.size - relocation.byte_offset < sizeof(char const*)) {
            throw std::runtime_error("module string relocation is outside its node config");
        }
        if (!relocation.string_data && relocation.string_size != 0)
            throw std::runtime_error("module string relocation has null data");
        if (!string_offsets[relocation.config_ordinal].insert(relocation.byte_offset).second)
            throw std::runtime_error("module has duplicate node config string relocations");
        ++string_counts[relocation.config_ordinal];
    }

    MaterializedNodeConfigs result;
    result.records.reserve(configs.size());
    result.storage.reserve(configs.size());
    std::vector<std::shared_ptr<OwnedNodeConfigBytes>> owned;
    owned.reserve(configs.size());
    for (std::size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
        auto const& config = configs[ordinal];
        if (!config.data || config.size == 0 || !is_valid_node_config_alignment(config.alignment)) {
            throw std::runtime_error("module node config has invalid storage");
        }
        auto bytes = std::make_shared<OwnedNodeConfigBytes>(config.size, config.alignment);
        std::memcpy(bytes->data, config.data, config.size);
        bytes->strings.reserve(string_counts[ordinal]);
        result.storage.emplace_back(bytes, bytes->data);
        result.records.push_back({
            .data = bytes->data,
            .size = config.size,
            .alignment = config.alignment,
        });
        owned.push_back(std::move(bytes));
    }

    for (auto const& relocation : relocations) {
        auto& bytes = *owned[relocation.config_ordinal];
        auto& string = bytes.strings.emplace_back();
        if (relocation.string_size != 0) {
            string.assign(relocation.string_data, relocation.string_size);
        }
        char const* data = string.data();
        std::memcpy(
            static_cast<std::byte*>(bytes.data) + relocation.byte_offset,
            &data,
            sizeof(data));
    }
    return result;
}

} // namespace iv::details
