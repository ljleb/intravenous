#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace iv::details {
constexpr size_t constexpr_hash_combine(size_t seed, size_t value)
{
    // The exact distribution is not externally observable. This is the
    // familiar boost-style combiner, chosen only to keep small structured
    // graph IDs from clustering on their first field.
    return seed ^ (value + static_cast<size_t>(0x9e3779b9) +
                   (seed << 6) + (seed >> 2));
}

// Deliberately small constexpr open-addressing containers. Lowering uses them
// only for build-local relations which need insert/find/contains and whose
// iteration order is irrelevant. They avoid the repeated element shifting of
// flat_map/flat_set during constant evaluation.
template<class Key, class Hash>
class ConstexprHashSet {
    std::vector<std::optional<Key>> buckets_;
    size_t size_ = 0;

    constexpr void rehash(size_t capacity)
    {
        std::vector<std::optional<Key>> old = std::move(buckets_);
        buckets_.assign(capacity, std::nullopt);
        size_ = 0;
        for (auto& value : old)
            if (value) insert_rehashed(std::move(*value));
    }

    constexpr void insert_rehashed(Key value)
    {
        auto slot = Hash{}(value) & (buckets_.size() - 1);
        while (buckets_[slot])
            slot = (slot + 1) & (buckets_.size() - 1);
        buckets_[slot].emplace(std::move(value));
        ++size_;
    }

    constexpr void ensure_capacity()
    {
        if (buckets_.empty()) {
            rehash(8);
        } else if ((size_ + 1) * 2 > buckets_.size()) {
            rehash(buckets_.size() * 2);
        }
    }

public:
    constexpr bool insert(Key value)
    {
        ensure_capacity();
        auto slot = Hash{}(value) & (buckets_.size() - 1);
        while (buckets_[slot]) {
            if (*buckets_[slot] == value) return false;
            slot = (slot + 1) & (buckets_.size() - 1);
        }
        buckets_[slot].emplace(std::move(value));
        ++size_;
        return true;
    }

    template<class Iterator>
    constexpr void insert_range(Iterator first, Iterator last)
    {
        for (; first != last; ++first) insert(*first);
    }

    constexpr bool contains(Key const& value) const
    {
        if (buckets_.empty()) return false;
        auto slot = Hash{}(value) & (buckets_.size() - 1);
        while (buckets_[slot]) {
            if (*buckets_[slot] == value) return true;
            slot = (slot + 1) & (buckets_.size() - 1);
        }
        return false;
    }

    constexpr size_t size() const { return size_; }
};

template<class Key, class Value, class Hash>
class ConstexprHashMap {
    struct Entry {
        Key key;
        Value value;
    };

    std::vector<std::optional<Entry>> buckets_;
    size_t size_ = 0;

    constexpr void rehash(size_t capacity)
    {
        std::vector<std::optional<Entry>> old = std::move(buckets_);
        buckets_.assign(capacity, std::nullopt);
        size_ = 0;
        for (auto& entry : old)
            if (entry) insert_rehashed(std::move(*entry));
    }

    constexpr void insert_rehashed(Entry entry)
    {
        auto slot = Hash{}(entry.key) & (buckets_.size() - 1);
        while (buckets_[slot])
            slot = (slot + 1) & (buckets_.size() - 1);
        buckets_[slot].emplace(std::move(entry));
        ++size_;
    }

    constexpr void ensure_capacity()
    {
        if (buckets_.empty()) {
            rehash(8);
        } else if ((size_ + 1) * 2 > buckets_.size()) {
            rehash(buckets_.size() * 2);
        }
    }

public:
    struct InsertResult {
        Value& value;
        bool inserted;
    };

    constexpr InsertResult try_emplace(Key key, Value value)
    {
        ensure_capacity();
        auto slot = Hash{}(key) & (buckets_.size() - 1);
        while (buckets_[slot]) {
            if (buckets_[slot]->key == key)
                return {buckets_[slot]->value, false};
            slot = (slot + 1) & (buckets_.size() - 1);
        }
        buckets_[slot].emplace(Entry{
            .key = std::move(key),
            .value = std::move(value),
        });
        ++size_;
        return {buckets_[slot]->value, true};
    }

    constexpr Value const* find(Key const& key) const
    {
        if (buckets_.empty()) return nullptr;
        auto slot = Hash{}(key) & (buckets_.size() - 1);
        while (buckets_[slot]) {
            if (buckets_[slot]->key == key) return &buckets_[slot]->value;
            slot = (slot + 1) & (buckets_.size() - 1);
        }
        return nullptr;
    }
};
} // namespace iv::details
