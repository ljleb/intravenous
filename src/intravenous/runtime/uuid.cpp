#include <intravenous/runtime/uuid.h>

#include <array>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_set>

namespace iv {
namespace {
std::unordered_set<std::string> &interned_strings()
{
    static auto *strings = new std::unordered_set<std::string>{""};
    return *strings;
}

std::mutex &interned_strings_mutex()
{
    static auto *mutex = new std::mutex;
    return *mutex;
}

char const *intern_impl(std::string value)
{
    std::scoped_lock lock(interned_strings_mutex());
    auto &strings = interned_strings();
    auto const [it, inserted] = strings.emplace(std::move(value));
    (void)inserted;
    return it->c_str();
}

std::string uuid_string_from_bytes(std::array<unsigned char, 16> bytes)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        out << std::setw(2) << static_cast<int>(bytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            out << '-';
        }
    }
    return out.str();
}
} // namespace

InternedString InternedString::from_string(std::string value)
{
    return InternedString(intern_impl(std::move(value)));
}

InternedString InternedString::from_view(std::string_view value)
{
    return from_string(std::string(value));
}

bool InternedString::empty() const
{
    return *value_ == '\0';
}

std::string_view InternedString::view() const
{
    return value_;
}

std::string InternedString::str() const
{
    return std::string(value_);
}

char const *InternedString::c_str() const
{
    return value_;
}

InternedString generate_uuid_v4()
{
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> dist(0, 255);

    std::array<unsigned char, 16> bytes {};
    for (auto &byte : bytes) {
        byte = static_cast<unsigned char>(dist(generator));
    }

    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

    return InternedString::from_string(uuid_string_from_bytes(bytes));
}
} // namespace iv
