#include <intravenous/runtime/configuration_expression.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace {
enum class Mode : unsigned { lowpass, highpass };
enum class Routing : unsigned { highpass = 7, direct = 9 };

struct Band {
    double frequency = 1000.0;
    double gain = 0.0;
};

struct FilterConfiguration {
    double cutoff = 440.0;
    Mode mode = Mode::lowpass;
    std::vector<Band> bands{};
};

using namespace iv::details;

ConfigurationExpressionTypeSchema const double_schema{
    .kind = ConfigurationExpressionTypeKind::floating_point,
    .size = sizeof(double),
    .alignment = alignof(double),
    .construct_scalar = &configuration_expression_details::construct_scalar<double>,
    .default_construct = &configuration_expression_details::default_construct<double>,
    .destroy = &configuration_expression_details::destroy<double>,
};

using FixedValues = std::array<double, 2>;
ConfigurationExpressionTypeSchema const fixed_values_schema{
    .kind = ConfigurationExpressionTypeKind::sequence,
    .size = sizeof(FixedValues),
    .alignment = alignof(FixedValues),
    .element_type = &double_schema,
    .fixed_element_count = 2,
    .default_construct =
        &configuration_expression_details::default_construct<FixedValues>,
    .destroy = &configuration_expression_details::destroy<FixedValues>,
    .assign_element =
        &configuration_expression_details::assign_element<FixedValues>,
};

ConfigurationTypeIdentity const fixed_values_identity{
    .nominal_id = "test.FixedValues",
    .nominal_id_size = 16,
    .definition_fingerprint = "fixed-values",
    .definition_fingerprint_size = 12,
    .display_name = "FixedValues",
    .display_name_size = 11,
    .expression_schema = &fixed_values_schema,
};

ConfigurationTypeIdentity const* fixed_values_parameter_types[]{
    &fixed_values_identity,
};
ConfigurationValueOperations const* fixed_values_parameter_operations[]{
    configuration_value_operations<FixedValues>(),
};
RegisteredSignature const fixed_values_signature{
    .parameter_types = fixed_values_parameter_types,
    .parameter_operations = fixed_values_parameter_operations,
    .required_argument_count = 1,
    .argument_count = 1,
};

std::array mode_enumerators{
    ConfigurationExpressionEnumeratorSchema{
        .name = "lowpass", .name_size = 7, .unsigned_value = 0},
    ConfigurationExpressionEnumeratorSchema{
        .name = "highpass", .name_size = 8, .unsigned_value = 1},
};

ConfigurationExpressionTypeSchema const mode_schema{
    .kind = ConfigurationExpressionTypeKind::enumeration,
    .size = sizeof(Mode),
    .alignment = alignof(Mode),
    .integer_is_signed = false,
    .integer_bit_width = sizeof(Mode) * 8,
    .enumerators = mode_enumerators.data(),
    .enumerator_count = mode_enumerators.size(),
    .construct_scalar = &configuration_expression_details::construct_scalar<Mode>,
    .default_construct = &configuration_expression_details::default_construct<Mode>,
    .destroy = &configuration_expression_details::destroy<Mode>,
};

std::array band_fields{
    ConfigurationExpressionFieldSchema{
        .name = "frequency", .name_size = 9,
        .byte_offset = offsetof(Band, frequency), .type = &double_schema,
        .assign = &configuration_expression_details::assign<double>},
    ConfigurationExpressionFieldSchema{
        .name = "gain", .name_size = 4,
        .byte_offset = offsetof(Band, gain), .type = &double_schema,
        .assign = &configuration_expression_details::assign<double>},
};

ConfigurationExpressionTypeSchema const band_schema{
    .kind = ConfigurationExpressionTypeKind::aggregate,
    .size = sizeof(Band),
    .alignment = alignof(Band),
    .fields = band_fields.data(),
    .field_count = band_fields.size(),
    .default_construct = &configuration_expression_details::default_construct<Band>,
    .destroy = &configuration_expression_details::destroy<Band>,
};

ConfigurationExpressionTypeSchema const bands_schema{
    .kind = ConfigurationExpressionTypeKind::sequence,
    .size = sizeof(std::vector<Band>),
    .alignment = alignof(std::vector<Band>),
    .element_type = &band_schema,
    .default_construct =
        &configuration_expression_details::default_construct<std::vector<Band>>,
    .destroy = &configuration_expression_details::destroy<std::vector<Band>>,
    .append_element =
        &configuration_expression_details::append_element<std::vector<Band>>,
};

std::array filter_fields{
    ConfigurationExpressionFieldSchema{
        .name = "cutoff", .name_size = 6,
        .byte_offset = offsetof(FilterConfiguration, cutoff), .type = &double_schema,
        .assign = &configuration_expression_details::assign<double>},
    ConfigurationExpressionFieldSchema{
        .name = "mode", .name_size = 4,
        .byte_offset = offsetof(FilterConfiguration, mode), .type = &mode_schema,
        .assign = &configuration_expression_details::assign<Mode>},
    ConfigurationExpressionFieldSchema{
        .name = "bands", .name_size = 5,
        .byte_offset = offsetof(FilterConfiguration, bands), .type = &bands_schema,
        .assign =
            &configuration_expression_details::assign<std::vector<Band>>},
};

ConfigurationExpressionTypeSchema const filter_schema{
    .kind = ConfigurationExpressionTypeKind::aggregate,
    .size = sizeof(FilterConfiguration),
    .alignment = alignof(FilterConfiguration),
    .fields = filter_fields.data(),
    .field_count = filter_fields.size(),
    .default_construct =
        &configuration_expression_details::default_construct<FilterConfiguration>,
    .destroy = &configuration_expression_details::destroy<FilterConfiguration>,
};

ConfigurationTypeIdentity const filter_identity{
    .nominal_id = "test.FilterConfiguration",
    .nominal_id_size = 24,
    .definition_fingerprint = "test",
    .definition_fingerprint_size = 4,
    .display_name = "FilterConfiguration",
    .display_name_size = 19,
    .expression_schema = &filter_schema,
};

ConfigurationTypeIdentity const* parameter_types[]{&filter_identity};
ConfigurationValueOperations const* parameter_operations[]{
    configuration_value_operations<FilterConfiguration>(),
};
RegisteredSignature const signature{
    .parameter_types = parameter_types,
    .parameter_operations = parameter_operations,
    .required_argument_count = 1,
    .argument_count = 1,
};

std::array routing_enumerators{
    ConfigurationExpressionEnumeratorSchema{
        .name = "highpass", .name_size = 8, .unsigned_value = 7},
    ConfigurationExpressionEnumeratorSchema{
        .name = "direct", .name_size = 6, .unsigned_value = 9},
};

ConfigurationExpressionTypeSchema const routing_schema{
    .kind = ConfigurationExpressionTypeKind::enumeration,
    .size = sizeof(Routing),
    .alignment = alignof(Routing),
    .integer_is_signed = false,
    .integer_bit_width = sizeof(Routing) * 8,
    .enumerators = routing_enumerators.data(),
    .enumerator_count = routing_enumerators.size(),
    .construct_scalar = &configuration_expression_details::construct_scalar<Routing>,
    .default_construct = &configuration_expression_details::default_construct<Routing>,
    .destroy = &configuration_expression_details::destroy<Routing>,
};

ConfigurationTypeIdentity const mode_identity{
    .nominal_id = "test.Mode",
    .nominal_id_size = 9,
    .definition_fingerprint = "mode",
    .definition_fingerprint_size = 4,
    .display_name = "Mode",
    .display_name_size = 4,
    .expression_schema = &mode_schema,
};

ConfigurationTypeIdentity const routing_identity{
    .nominal_id = "test.Routing",
    .nominal_id_size = 12,
    .definition_fingerprint = "routing",
    .definition_fingerprint_size = 7,
    .display_name = "Routing",
    .display_name_size = 7,
    .expression_schema = &routing_schema,
};

ConfigurationTypeIdentity const* enum_parameter_types[]{
    &mode_identity,
    &routing_identity,
};
ConfigurationValueOperations const* enum_parameter_operations[]{
    configuration_value_operations<Mode>(),
    configuration_value_operations<Routing>(),
};
RegisteredSignature const enum_signature{
    .parameter_types = enum_parameter_types,
    .parameter_operations = enum_parameter_operations,
    .required_argument_count = 2,
    .argument_count = 2,
};

ConfigurationExpressionTypeSchema const node_reference_schema{
    .kind = ConfigurationExpressionTypeKind::node_reference,
    .size = sizeof(void*),
    .alignment = alignof(void*),
};

ConfigurationTypeIdentity const node_reference_identity{
    .nominal_id = "test.NodeRef",
    .nominal_id_size = 12,
    .definition_fingerprint = "node-ref",
    .definition_fingerprint_size = 8,
    .display_name = "NodeRef",
    .display_name_size = 7,
    .expression_schema = &node_reference_schema,
};

ConfigurationTypeIdentity const* node_reference_parameter_types[]{
    &node_reference_identity,
};
ConfigurationValueOperations const* node_reference_parameter_operations[]{nullptr};
RegisteredSignature const node_reference_signature{
    .parameter_types = node_reference_parameter_types,
    .parameter_operations = node_reference_parameter_operations,
    .required_argument_count = 1,
    .argument_count = 1,
};
} // namespace

TEST(ConfigurationExpression, ConstructsNestedNamedAggregatesAndSequences)
{
    auto values = iv::compile_configuration_expression_list(R"(
        {
            .cutoff = max(1200, 600 * 3),
            .mode = highpass,
            .bands = {
                {.frequency = 200, .gain = -3},
                {.frequency = 2000, .gain = 2.5}
            }
        }
    )", signature);
    ASSERT_EQ(values.arguments().size(), 1u);
    auto const& value = *static_cast<FilterConfiguration const*>(
        values.arguments()[0].data);
    EXPECT_DOUBLE_EQ(value.cutoff, 1800.0);
    EXPECT_EQ(value.mode, Mode::highpass);
    ASSERT_EQ(value.bands.size(), 2u);
    EXPECT_DOUBLE_EQ(value.bands[0].frequency, 200.0);
    EXPECT_DOUBLE_EQ(value.bands[0].gain, -3.0);
    EXPECT_DOUBLE_EQ(value.bands[1].frequency, 2000.0);
    EXPECT_DOUBLE_EQ(value.bands[1].gain, 2.5);
}

TEST(ConfigurationExpression, AssignsFixedSequenceElementsThroughProviderCode)
{
    auto values = iv::compile_configuration_expression_list(
        "{1.25, 2.5}", fixed_values_signature);
    ASSERT_EQ(values.arguments().size(), 1u);
    auto const& value = *static_cast<FixedValues const*>(
        values.arguments()[0].data);
    EXPECT_DOUBLE_EQ(value[0], 1.25);
    EXPECT_DOUBLE_EQ(value[1], 2.5);
}

TEST(ConfigurationExpression, RejectsValuesOutsideTheTransportedFloatingRange)
{
    EXPECT_THROW(
        (void)iv::compile_configuration_expression_list(
            "{.cutoff = 1e309}", signature),
        std::invalid_argument);
}

TEST(ConfigurationExpression, ParsesStableNodeReferencesAndAllPathSelectors)
{
    auto parsed = iv::parse_configuration_expression_list(
        "&550e8400-e29b-41d4-a716-446655440000.processor[2]#right");
    ASSERT_EQ(parsed.arguments.size(), 1u);
    auto const* reference = std::get_if<iv::ConfigurationNodeReferenceExpression>(
        &parsed.arguments.front()->value);
    ASSERT_NE(reference, nullptr);
    EXPECT_EQ(reference->instance_id, "550e8400-e29b-41d4-a716-446655440000");
    ASSERT_EQ(reference->path.size(), 3u);
    EXPECT_EQ(
        std::get<iv::ProjectVirtualNodeSelector>(reference->path[0]).source_identity,
        "processor");
    EXPECT_EQ(std::get<iv::ProjectVirtualMemberSelector>(reference->path[1]).index, 2u);
    EXPECT_EQ(std::get<iv::ProjectTiledChildSelector>(reference->path[2]).name, "right");
}

TEST(ConfigurationExpression, ResolvesTheSameEnumeratorNameFromEachExpectedType)
{
    auto values = iv::compile_configuration_expression_list(
        "highpass, highpass", enum_signature);
    ASSERT_EQ(values.arguments().size(), 2u);
    EXPECT_EQ(
        *static_cast<Mode const*>(values.arguments()[0].data),
        Mode::highpass);
    EXPECT_EQ(
        *static_cast<Routing const*>(values.arguments()[1].data),
        Routing::highpass);
}

TEST(ConfigurationExpression, RejectsUnknownEnumeratorForTheExpectedType)
{
    EXPECT_THROW(
        (void)iv::compile_configuration_expression_list(
            "direct, lowpass", enum_signature),
        std::invalid_argument);
}

TEST(ConfigurationExpression, RejectsNonCanonicalNodeIdLiteral)
{
    EXPECT_THROW(
        (void)iv::parse_configuration_expression_list("&550e8400-filter"),
        std::invalid_argument);
    EXPECT_THROW(
        (void)iv::parse_configuration_expression_list(
            "&550E8400-e29b-41d4-a716-446655440000"),
        std::invalid_argument);
}

TEST(ConfigurationExpression, RejectsStandaloneTiledChildSelector)
{
    EXPECT_THROW(
        (void)iv::parse_configuration_expression_list("#left"),
        std::invalid_argument);
}

TEST(ConfigurationExpression, RefusesToInventACrossSessionLiveNodeHandle)
{
    try {
        (void)iv::compile_configuration_expression_list(
            "&550e8400-e29b-41d4-a716-446655440000",
            node_reference_signature);
        FAIL() << "expected node-reference materialization to fail";
    } catch (std::invalid_argument const& error) {
        EXPECT_NE(
            std::string_view(error.what()).find("Front 12"),
            std::string_view::npos);
    }
}
