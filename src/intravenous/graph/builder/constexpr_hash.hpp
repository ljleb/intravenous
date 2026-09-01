#pragma once

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>
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

struct ConstexprIdentityHash {
    constexpr size_t operator()(size_t value) const { return value; }
};

struct ConstexprStringHash {
    constexpr size_t operator()(std::string const& value) const
    {
        size_t seed = 0;
        for (char c : value)
            seed = constexpr_hash_combine(seed, static_cast<size_t>(c));
        return seed;
    }
};

// Bucket-order iterator over an open-addressing container's slot vector.
// Skips empty slots so iteration yields exactly the stored elements.
template<class BucketVector, class Value>
class ConstexprBucketIterator {
    BucketVector* buckets_ = nullptr;
    size_t slot_ = 0;

    constexpr void skip_empty()
    {
        if (!buckets_) return;
        while (slot_ < buckets_->size() && !(*buckets_)[slot_])
            ++slot_;
    }

public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Value;
    using difference_type = std::ptrdiff_t;
    using pointer = Value const*;
    using reference = Value const&;

    constexpr ConstexprBucketIterator() = default;
    constexpr ConstexprBucketIterator(BucketVector& buckets, size_t slot)
        : buckets_(&buckets), slot_(slot) { skip_empty(); }

    constexpr reference operator*() const { return (*buckets_)[slot_].value(); }
    constexpr pointer operator->() const { return &(*buckets_)[slot_].value(); }

    constexpr ConstexprBucketIterator& operator++() {
        ++slot_;
        skip_empty();
        return *this;
    }
    constexpr ConstexprBucketIterator operator++(int) {
        auto tmp = *this;
        ++*this;
        return tmp;
    }

    constexpr bool operator==(ConstexprBucketIterator const& other) const {
        return slot_ == other.slot_
            && (buckets_ == other.buckets_
                || (!buckets_ && !other.buckets_));
    }
    constexpr bool operator!=(ConstexprBucketIterator const& other) const {
        return !(*this == other);
    }
};

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
    using const_iterator = ConstexprBucketIterator<
        std::vector<std::optional<Key>> const, Key>;

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

    constexpr bool emplace(Key value) { return insert(std::move(value)); }

    template<class Iterator>
    constexpr void insert_range(Iterator first, Iterator last)
    {
        for (; first != last; ++first) insert(*first);
    }

    constexpr Key const* find(Key const& value) const
    {
        if (buckets_.empty()) return nullptr;
        auto slot = Hash{}(value) & (buckets_.size() - 1);
        while (buckets_[slot]) {
            if (*buckets_[slot] == value) return &*buckets_[slot];
            slot = (slot + 1) & (buckets_.size() - 1);
        }
        return nullptr;
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

    constexpr const_iterator begin() const
    { return {buckets_, 0}; }
    constexpr const_iterator end() const
    { return {buckets_, buckets_.size()}; }
    constexpr size_t size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }
    constexpr void clear() { buckets_.clear(); size_ = 0; }
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

    using const_iterator = ConstexprBucketIterator<
        std::vector<std::optional<Entry>> const, Entry>;

    constexpr Value& operator[](Key const& key)
    {
        return try_emplace(key, Value{}).value;
    }

    constexpr Value& operator[](Key&& key)
    {
        return try_emplace(std::move(key), Value{}).value;
    }

    constexpr InsertResult emplace(Key key, Value value)
    {
        return try_emplace(std::move(key), std::move(value));
    }

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

    constexpr Value* find(Key const& key)
    {
        if (buckets_.empty()) return nullptr;
        auto slot = Hash{}(key) & (buckets_.size() - 1);
        while (buckets_[slot]) {
            if (buckets_[slot]->key == key) return &buckets_[slot]->value;
            slot = (slot + 1) & (buckets_.size() - 1);
        }
        return nullptr;
    }

    constexpr bool contains(Key const& key) const { return find(key) != nullptr; }

    constexpr const_iterator begin() const
    { return {buckets_, 0}; }
    constexpr const_iterator end() const
    { return {buckets_, buckets_.size()}; }
    constexpr size_t size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }
    constexpr void clear() { buckets_.clear(); size_ = 0; }
};
} // namespace iv::details
