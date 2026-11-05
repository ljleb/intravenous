#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace iv {
// One language-neutral package declaration that a frontend can match to a
// presentation provider that it owns. The backend never interprets the
// provider ID as a component or source-language name.
struct NodePresentationDescriptor {
    std::string definition_id{};
    std::string presentation_id{};
    std::string display_name{};

    friend bool operator==(
        NodePresentationDescriptor const&,
        NodePresentationDescriptor const&) = default;
};

namespace details {
// Trivial package ABI record emitted by IV_NODE_PRESENTATION. The package
// finalizer collects these records into the finalized package table.
struct PackageNodePresentation {
    char const* definition_id = nullptr;
    std::size_t definition_id_size = 0;
    char const* presentation_id = nullptr;
    std::size_t presentation_id_size = 0;
    char const* display_name = nullptr;
    std::size_t display_name_size = 0;
};

static_assert(std::is_standard_layout_v<PackageNodePresentation>);
static_assert(std::is_trivially_copyable_v<PackageNodePresentation>);

#define IV_PACKAGE_NODE_PRESENTATION_SECTION_NAME \
    "iv_package_node_presentation_records"

inline constexpr std::string_view package_node_presentation_section =
    IV_PACKAGE_NODE_PRESENTATION_SECTION_NAME;
inline constexpr std::string_view package_node_presentations_abi_symbol =
    "iv_package_node_presentations";
static_assert(
    package_node_presentation_section != package_node_presentations_abi_symbol);
} // namespace details
} // namespace iv

#if defined(__clang__)
#define IV_PACKAGE_NODE_PRESENTATION_RECORD \
    __attribute__((used, section(IV_PACKAGE_NODE_PRESENTATION_SECTION_NAME)))
#else
#error "IV package node presentation registration requires Clang"
#endif

#define IV_NODE_PRESENTATION_CONCAT_INNER(a, b) a##b
#define IV_NODE_PRESENTATION_CONCAT(a, b) \
    IV_NODE_PRESENTATION_CONCAT_INNER(a, b)

// DefinitionId names an IV_NODE or IV_MODULE in the same package.
// PresentationId is a nominal compatibility key understood by frontend-owned
// providers. DisplayName is diagnostic text only.
#define IV_NODE_PRESENTATION(DefinitionId, PresentationId, DisplayName) \
    IV_NODE_PRESENTATION_IMPL( \
        DefinitionId, PresentationId, DisplayName, __COUNTER__)
#define IV_NODE_PRESENTATION_IMPL( \
    DefinitionId, PresentationId, DisplayName, Unique) \
    namespace { \
    IV_PACKAGE_NODE_PRESENTATION_RECORD constinit const \
        ::iv::details::PackageNodePresentation \
        IV_NODE_PRESENTATION_CONCAT(iv_package_node_presentation_, Unique){ \
            DefinitionId, sizeof(DefinitionId) - 1, \
            PresentationId, sizeof(PresentationId) - 1, \
            DisplayName, sizeof(DisplayName) - 1}; \
    }
