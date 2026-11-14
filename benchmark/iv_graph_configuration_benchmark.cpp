// Graph-configuration baseline benchmark.
//
// Measures the host-side builder/configuration/cache path that the scoped
// BuilderSession migration (docs/scoped_graph_builder_and_subgraph_closure_direction.md,
// implementation Front 12) reshapes. Package definitions are host function
// pointers, so no package compilation or ORC loading is measured: every
// microsecond reported here is GraphBuilder, BuilderSession, NodeInstances
// cache lookup and reuse, or root finalization.
//
// Each scenario names the configuration behavior it exercises. The baseline
// taken before the scoped-builder migration, and the mechanisms it measured,
// are recorded under Front 12 in docs/implementation_fronts_direction.md.
//
//   builder_direct        GraphBuilder + configure_concrete_node with no package
//                         table: graph-table cost per node and connection.
//   tree_cold             NodeInstances::configure_and_embed on a fresh
//                         NodeInstances. Every distinct registered-module call is
//                         a configuration-cache miss; equal calls are hits.
//   tree_warm             Same request against a primed NodeInstances: the
//                         top-level call is one hit placing the complete tree.
//   tree_parent_miss      Primed NodeInstances, unique top-level argument per
//                         repetition: the root module misses, every nested call
//                         hits.
//   tree_uncached         Arguments without equality/hash. Every module call at
//                         every depth is constructed on every repetition.
//   subgraphs_cold        One module with repeated GraphBuilder::subgraph
//                         lambdas.
//   tiled_cold/_warm      A registered module requested with a stereo tiled
//                         layout: one module body per member plus interface
//                         validation.
//   instances_warm        Many top-level instances of one primed definition.
//
// Output is one line per scenario, key=value, microseconds unless noted.

#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/constant.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/module/configuration_argument.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/node/compiler_record.h>
#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using iv::details::ConfigurationArgument;
using iv::details::ConfigurationTypeIdentity;
using iv::details::ConfigurationValueOperations;
using iv::details::PackageDefinition;
using iv::details::PackageDefinitionKind;
using iv::details::RegisteredSignature;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Benchmark shape. Providers are plain function pointers, so the shape is
// process-global configuration read by the provider bodies.

struct Shape {
    int depth = 3;
    int fanout = 3;
    int leaves = 8;
    int subgraphs = 32;
    int instances = 64;
    int direct_nodes = 1000;
    // Extra unused leaf definitions in the package revision. Every
    // configure_cached miss copies the complete package tables in
    // set_builder_packages and find_builder_definition scans them linearly,
    // so a realistic package size matters.
    int padding_definitions = 200;
};
Shape shape{};
std::size_t module_invocations = 0;
std::size_t leaf_invocations = 0;

// ---------------------------------------------------------------------------
// Configuration types and signatures.

constexpr char package_root_text[] = "/tmp/iv-graph-configuration-benchmark";
constexpr char source_file_text[] = "/tmp/iv-graph-configuration-benchmark/module.cpp";
constexpr char int_nominal[] = "benchmark:int";
constexpr char int_fingerprint[] = "benchmark:int:v1";
constexpr char int_display[] = "int";

iv::details::ConfigurationExpressionTypeSchema const int_expression_schema{
    .kind = iv::details::ConfigurationExpressionTypeKind::signed_integer,
    .size = sizeof(int),
    .alignment = alignof(int),
    .integer_is_signed = true,
    .integer_bit_width = sizeof(int) * 8,
    .construct_scalar =
        &iv::details::configuration_expression_details::construct_scalar<int>,
    .default_construct =
        &iv::details::configuration_expression_details::default_construct<int>,
    .destroy = &iv::details::configuration_expression_details::destroy<int>,
};

ConfigurationTypeIdentity const int_identity{
    .nominal_id = int_nominal,
    .nominal_id_size = sizeof(int_nominal) - 1,
    .definition_fingerprint = int_fingerprint,
    .definition_fingerprint_size = sizeof(int_fingerprint) - 1,
    .display_name = int_display,
    .display_name_size = sizeof(int_display) - 1,
    .expression_schema = &int_expression_schema,
};

// Copyable/destructible but without equality/hash: constructible and
// retainable, not cache-keyable.
ConfigurationValueOperations const uncached_int_operations{
    .size = sizeof(int),
    .alignment = alignof(int),
    .copy_construct = [](void* destination, void const* source) {
        ::new (destination) int(*static_cast<int const*>(source));
    },
    .destroy = [](void* value) noexcept {
        std::destroy_at(static_cast<int*>(value));
    },
};

ConfigurationTypeIdentity const* const two_int_types[]{&int_identity, &int_identity};
ConfigurationValueOperations const* const two_int_operations[]{
    iv::details::configuration_value_operations<int>(),
    iv::details::configuration_value_operations<int>(),
};
ConfigurationValueOperations const* const two_uncached_int_operations[]{
    &uncached_int_operations,
    &uncached_int_operations,
};

RegisteredSignature const zero_signature{};
RegisteredSignature const two_int_signature{
    .parameter_types = two_int_types,
    .parameter_operations = two_int_operations,
    .required_argument_count = 2,
    .argument_count = 2,
};
RegisteredSignature const two_uncached_int_signature{
    .parameter_types = two_int_types,
    .parameter_operations = two_uncached_int_operations,
    .required_argument_count = 2,
    .argument_count = 2,
};

RegisteredSignature const* zero_signature_callback() { return &zero_signature; }
RegisteredSignature const* two_int_signature_callback() { return &two_int_signature; }
RegisteredSignature const* two_uncached_int_signature_callback()
{
    return &two_uncached_int_signature;
}

ConfigurationArgument int_argument(int& value)
{
    return ConfigurationArgument{
        .data = &value,
        .type = &int_identity,
    };
}

int int_value(ConfigurationArgument const& argument)
{
    return *static_cast<int const*>(argument.data);
}

// ---------------------------------------------------------------------------
// Definitions.

constexpr char constant_id[] = "iv.benchmark.constant";
constexpr char subtract_id[] = "iv.benchmark.subtract";
// Configuration-cycle detection (NodeInstances' ConfigurationStackEntry and
// BuilderSession's module stack) is keyed by definition ID alone, so a module
// may not configure itself even with different arguments. Each tree level is
// therefore its own definition: tree_ids[level] configures tree_ids[level - 1].
// The vectors are sized once in make_snapshot() and never reallocated, because
// PackageDefinition stores pointers into these strings.
std::vector<std::string> tree_ids{};
std::vector<std::string> uncached_tree_ids{};
constexpr char subgraphs_id[] = "iv.benchmark.subgraphs";

iv::NodeRef leaf(std::string_view id, iv::GraphBuilder& graph)
{
    return iv::details::configure_package_definition(graph, id, {});
}

iv::NodeRef constant_leaf(
    iv::GraphBuilder& graph, std::span<ConfigurationArgument>, iv::ChannelLayout const* layout)
{
    ++leaf_invocations;
    if (layout) throw std::invalid_argument("benchmark leaf has no tiled form");
    return iv::details::configure_concrete_node<iv::Constant>(
        graph, iv::Sample{0.25f}).node_ref();
}

iv::NodeRef subtract_leaf(
    iv::GraphBuilder& graph, std::span<ConfigurationArgument>, iv::ChannelLayout const* layout)
{
    ++leaf_invocations;
    if (layout) throw std::invalid_argument("benchmark leaf has no tiled form");
    return iv::details::configure_concrete_node<iv::Subtract>(graph).node_ref();
}

// A chain of `leaves` constant/subtract pairs feeding from `head`.
iv::SamplePortRef leaf_chain(iv::GraphBuilder& graph, iv::SamplePortRef head)
{
    for (int index = 0; index < shape.leaves; ++index) {
        auto const value = leaf(constant_id, graph);
        auto const difference = leaf(subtract_id, graph);
        difference(value, head);
        head = static_cast<iv::SamplePortRef>(difference);
    }
    return head;
}

void tree_body(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument> arguments,
    std::vector<std::string> const& level_ids)
{
    using namespace iv;
    ++module_invocations;
    auto const level = int_value(arguments[0]);
    auto head = leaf_chain(graph, static_cast<SamplePortRef>(leaf(constant_id, graph)));
    if (level > 0) {
        for (int branch = 0; branch < shape.fanout; ++branch) {
            int child_level = level - 1;
            // Sibling subtrees with equal (level, branch) are equal cache keys,
            // so a cold tree contains both misses and same-batch hits.
            int child_salt = branch;
            std::array child_arguments{
                int_argument(child_level), int_argument(child_salt)};
            auto const child = details::configure_package_definition(
                graph,
                level_ids.at(static_cast<std::size_t>(child_level)),
                std::span<ConfigurationArgument>(child_arguments));
            auto const merge = leaf(subtract_id, graph);
            merge(child, head);
            head = static_cast<SamplePortRef>(merge);
        }
    }
    graph.outputs("out"_P = head);
}

void tree_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument> arguments)
{
    tree_body(graph, arguments, tree_ids);
}

void uncached_tree_module(
    iv::GraphBuilder& graph, std::span<ConfigurationArgument> arguments)
{
    tree_body(graph, arguments, uncached_tree_ids);
}

void subgraphs_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    using namespace iv;
    ++module_invocations;
    auto head = static_cast<SamplePortRef>(leaf(constant_id, graph));
    for (int index = 0; index < shape.subgraphs; ++index) {
        auto const nested = graph.subgraph([&](SubgraphBuilder& boundary) {
            auto const input = static_cast<SamplePortRef>(boundary.input());
            boundary.outputs("out"_P = leaf_chain(graph, input));
        });
        nested(head);
        head = static_cast<SamplePortRef>(nested);
    }
    graph.outputs("out"_P = head);
}

struct DefinitionSpec {
    std::string_view id{};
    PackageDefinitionKind kind = PackageDefinitionKind::module;
    iv::details::IvModuleConfigureFunction module_build = nullptr;
    iv::details::NodeTypeConfigureFunction node_build = nullptr;
    iv::details::RegisteredSignatureFunction signature = nullptr;
    iv::details::NodeCompilerRecord const* compiler_record = nullptr;
};

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_snapshot()
{
    std::vector<DefinitionSpec> specs{
        DefinitionSpec{
            .id = constant_id,
            .kind = PackageDefinitionKind::node,
            .node_build = &constant_leaf,
            .signature = &zero_signature_callback,
            .compiler_record = &iv::details::node_compiler_record<iv::Constant>,
        },
        DefinitionSpec{
            .id = subtract_id,
            .kind = PackageDefinitionKind::node,
            .node_build = &subtract_leaf,
            .signature = &zero_signature_callback,
            .compiler_record = &iv::details::node_compiler_record<iv::Subtract>,
        },
        DefinitionSpec{
            .id = subgraphs_id,
            .module_build = &subgraphs_module,
            .signature = &zero_signature_callback,
        },
    };
    auto const levels = static_cast<std::size_t>(shape.depth) + 1;
    tree_ids.clear();
    uncached_tree_ids.clear();
    tree_ids.reserve(levels);
    uncached_tree_ids.reserve(levels);
    for (std::size_t level = 0; level < levels; ++level) {
        tree_ids.push_back("iv.benchmark.tree." + std::to_string(level));
        uncached_tree_ids.push_back(
            "iv.benchmark.uncached_tree." + std::to_string(level));
        specs.push_back(DefinitionSpec{
            .id = tree_ids.back(),
            .module_build = &tree_module,
            .signature = &two_int_signature_callback,
        });
        specs.push_back(DefinitionSpec{
            .id = uncached_tree_ids.back(),
            .module_build = &uncached_tree_module,
            .signature = &two_uncached_int_signature_callback,
        });
    }

    auto revision = std::make_shared<iv::PackageRevision>();
    revision->package_id = "iv.benchmark.package";
    revision->package_root = package_root_text;
    revision->revision = 1;
    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = 1;
    for (auto const& spec : specs) {
        revision->provider_definitions.push_back(PackageDefinition{
            .kind = spec.kind,
            .id = spec.id.data(),
            .id_size = spec.id.size(),
            .source_file = source_file_text,
            .source_file_size = sizeof(source_file_text) - 1,
            .package_root = package_root_text,
            .package_root_size = sizeof(package_root_text) - 1,
            .module_build = spec.module_build,
            .node_build = spec.node_build,
            .node_compiler_record = spec.compiler_record,
            .signature = spec.signature,
        });
        auto const provider = iv::NodeDefinitionProvider{
            .module_build = spec.module_build,
            .leaf_build = spec.node_build,
            .signature = spec.signature,
        };
        if (spec.kind == PackageDefinitionKind::module) {
            snapshot->by_id.emplace(std::string(spec.id), iv::NodeDefinitionEntry{
                .definition_id = std::string(spec.id),
                .kind = iv::NodeDefinitionKind::module,
                .version = 1,
                .definition = iv::ModuleNodeDefinition{
                    .definition_id = std::string(spec.id),
                    .package_id = revision->package_id,
                    .package_root = revision->package_root,
                    .module_id = std::string(spec.id),
                    .provider = provider,
                },
            });
        } else {
            snapshot->by_id.emplace(std::string(spec.id), iv::NodeDefinitionEntry{
                .definition_id = std::string(spec.id),
                .kind = iv::NodeDefinitionKind::leaf,
                .version = 1,
                .definition = iv::LeafNodeDefinition{
                    .definition_id = std::string(spec.id),
                    .package_id = revision->package_id,
                    .package_root = revision->package_root,
                    .provider = provider,
                    .compiler_record = *spec.compiler_record,
                },
            });
        }
    }
    static std::vector<std::string> padding_ids;
    padding_ids.clear();
    padding_ids.reserve(static_cast<std::size_t>(shape.padding_definitions));
    for (int index = 0; index < shape.padding_definitions; ++index) {
        padding_ids.push_back("iv.benchmark.padding." + std::to_string(index));
        revision->provider_definitions.push_back(PackageDefinition{
            .kind = PackageDefinitionKind::node,
            .id = padding_ids.back().data(),
            .id_size = padding_ids.back().size(),
            .source_file = source_file_text,
            .source_file_size = sizeof(source_file_text) - 1,
            .package_root = package_root_text,
            .package_root_size = sizeof(package_root_text) - 1,
            .node_build = &constant_leaf,
            .node_compiler_record = &iv::details::node_compiler_record<iv::Constant>,
            .signature = &zero_signature_callback,
        });
    }
    snapshot->package_revisions.push_back(std::move(revision));
    return snapshot;
}

// ---------------------------------------------------------------------------
// Measurement.

struct Measurement {
    std::int64_t configure_us = 0;
    std::int64_t finish_us = 0;
    std::size_t bundles = 0;
    std::size_t module_invocations = 0;
    std::size_t leaf_invocations = 0;
};

struct Statistics {
    std::int64_t min = 0;
    std::int64_t median = 0;
    std::int64_t max = 0;
};

Statistics statistics(std::vector<std::int64_t> values)
{
    if (values.empty()) return {};
    std::ranges::sort(values);
    return {
        .min = values.front(),
        .median = values[values.size() / 2],
        .max = values.back(),
    };
}

std::int64_t elapsed_us(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
}

struct Request {
    std::string definition_id{};
    int level = 0;
    int salt = 0;
    std::optional<iv::ChannelLayout> tiled_layout{};
    std::size_t count = 1;
};

// One ProjectGraph::rebuild_locked-shaped configuration: fresh root builder,
// NodeInstances::configure_and_embed, root outputs(), consuming finish().
Measurement configure_once(
    iv::NodeInstances& instances,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& snapshot,
    Request const& request)
{
    std::vector<std::array<int, 2>> values(request.count, {request.level, request.salt});
    std::vector<std::array<ConfigurationArgument, 2>> arguments;
    std::vector<iv::NodeInstanceConfigurationRequest> requests;
    arguments.reserve(request.count);
    requests.reserve(request.count);
    bool const takes_arguments = request.definition_id != subgraphs_id;
    for (std::size_t index = 0; index < request.count; ++index) {
        arguments.push_back({int_argument(values[index][0]), int_argument(values[index][1])});
        requests.push_back(iv::NodeInstanceConfigurationRequest{
            .instance_id = "instance-" + std::to_string(index),
            .definition_id = request.definition_id,
            .arguments = takes_arguments
                ? std::span<ConfigurationArgument>(arguments.back())
                : std::span<ConfigurationArgument>{},
            .tiled_layout = request.tiled_layout,
        });
    }

    module_invocations = 0;
    leaf_invocations = 0;
    iv::GraphBuilder root;
    auto const configure_begin = Clock::now();
    auto const result = instances.configure_and_embed(snapshot, root, requests);
    auto const configure_end = Clock::now();
    if (!result.diagnostics.empty()) {
        throw std::runtime_error(
            "configuration failed for " + result.diagnostics.front().instance_id
            + ": " + result.diagnostics.front().message);
    }
    root.outputs();
    auto const finish_begin = Clock::now();
    auto const graph = std::move(root).finish();
    auto const finish_end = Clock::now();
    return {
        .configure_us = elapsed_us(configure_begin, configure_end),
        .finish_us = elapsed_us(finish_begin, finish_end),
        .bundles = graph.node_bundles.size(),
        .module_invocations = module_invocations,
        .leaf_invocations = leaf_invocations,
    };
}

Measurement builder_direct_once()
{
    using namespace iv;
    GraphBuilder graph;
    auto const begin = Clock::now();
    auto head = static_cast<SamplePortRef>(
        details::configure_concrete_node<Constant>(graph, Sample{0.25f}));
    for (int index = 0; index < shape.direct_nodes; ++index) {
        auto const value = details::configure_concrete_node<Constant>(graph, Sample{0.5f});
        auto const difference = details::configure_concrete_node<Subtract>(graph);
        difference(value, head);
        head = static_cast<SamplePortRef>(difference);
    }
    graph.outputs("out"_P = head);
    auto const end = Clock::now();
    auto const finish_begin = Clock::now();
    auto const configured = std::move(graph).finish();
    auto const finish_end = Clock::now();
    return {
        .configure_us = elapsed_us(begin, end),
        .finish_us = elapsed_us(finish_begin, finish_end),
        .bundles = configured.node_bundles.size(),
    };
}

struct Scenario {
    std::string_view name{};
    std::function<Measurement(std::size_t repetition)> run{};
    std::function<std::size_t()> cache_entries{};
};

void report(
    Scenario const& scenario,
    std::size_t warmup,
    std::size_t repetitions)
{
    for (std::size_t index = 0; index < warmup; ++index) (void)scenario.run(index);
    std::vector<std::int64_t> configure;
    std::vector<std::int64_t> finish;
    std::vector<std::int64_t> total;
    Measurement last{};
    for (std::size_t index = 0; index < repetitions; ++index) {
        last = scenario.run(warmup + index);
        configure.push_back(last.configure_us);
        finish.push_back(last.finish_us);
        total.push_back(last.configure_us + last.finish_us);
    }
    auto const c = statistics(std::move(configure));
    auto const f = statistics(std::move(finish));
    auto const t = statistics(std::move(total));
    std::cout << "scenario=" << scenario.name
              << " reps=" << repetitions
              << " bundles=" << last.bundles
              << " module_invocations=" << last.module_invocations
              << " leaf_invocations=" << last.leaf_invocations
              << " configure_us_min=" << c.min
              << " configure_us_median=" << c.median
              << " configure_us_max=" << c.max
              << " finish_us_median=" << f.median
              << " total_us_median=" << t.median;
    if (scenario.cache_entries) {
        std::cout << " cache_entries=" << scenario.cache_entries();
    }
    std::cout << '\n';
}

struct Options {
    std::size_t repetitions = 30;
    std::size_t warmup = 3;
    std::vector<std::string> scenarios{};
};

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto value = [&]() -> std::string {
            if (++i == argc) throw std::runtime_error("missing value for " + std::string(arg));
            return argv[i];
        };
        if (arg == "--reps") options.repetitions = std::stoull(value());
        else if (arg == "--warmup") options.warmup = std::stoull(value());
        else if (arg == "--depth") shape.depth = std::stoi(value());
        else if (arg == "--fanout") shape.fanout = std::stoi(value());
        else if (arg == "--leaves") shape.leaves = std::stoi(value());
        else if (arg == "--subgraphs") shape.subgraphs = std::stoi(value());
        else if (arg == "--instances") shape.instances = std::stoi(value());
        else if (arg == "--direct-nodes") shape.direct_nodes = std::stoi(value());
        else if (arg == "--padding-definitions") shape.padding_definitions = std::stoi(value());
        else if (arg == "--scenario") options.scenarios.push_back(value());
        else if (arg == "--help") {
            std::cout
                << "Usage: iv_graph_configuration_benchmark [--reps N] [--warmup N]"
                << " [--depth N] [--fanout N] [--leaves N] [--subgraphs N]"
                << " [--instances N] [--direct-nodes N] [--padding-definitions N]"
                << " [--scenario NAME]...\n"
                << "Scenarios: builder_direct tree_cold tree_warm tree_parent_miss"
                << " tree_uncached subgraphs_cold tiled_cold tiled_warm"
                << " instances_warm\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument '" + std::string(arg) + "'");
        }
    }
    if (shape.depth < 0 || shape.fanout < 0 || shape.leaves < 0
        || shape.subgraphs < 0 || shape.instances < 1 || shape.direct_nodes < 0
        || shape.padding_definitions < 0) {
        throw std::runtime_error("benchmark shape values must be non-negative");
    }
    return options;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        auto const options = parse_options(argc, argv);
        auto const snapshot = make_snapshot();
        iv::ChannelLayout const stereo{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        };

        // Each warm scenario owns one NodeInstances so cache contents never
        // leak between scenarios.
        auto make_warm = [&](Request request) {
            auto instances = std::make_shared<iv::NodeInstances>();
            (void)configure_once(*instances, snapshot, request);
            return instances;
        };
        auto cold = [&](Request request) {
            return [&, request](std::size_t) {
                iv::NodeInstances instances;
                return configure_once(instances, snapshot, request);
            };
        };
        auto cache_size = [](std::shared_ptr<iv::NodeInstances> const& instances) {
            return [instances] { return instances->configuration_cache_size(); };
        };

        auto const depth = static_cast<std::size_t>(shape.depth);
        auto const instance_level = std::min(1, shape.depth);
        Request const tree{.definition_id = tree_ids[depth], .level = shape.depth};
        Request const uncached{
            .definition_id = uncached_tree_ids[depth], .level = shape.depth};
        Request const subgraphs{.definition_id = subgraphs_id};
        Request const tiled{
            .definition_id = tree_ids[depth],
            .level = shape.depth,
            .tiled_layout = stereo,
        };
        Request const instances_request{
            .definition_id = tree_ids[static_cast<std::size_t>(instance_level)],
            .level = instance_level,
            .count = static_cast<std::size_t>(shape.instances),
        };

        auto const warm_tree = make_warm(tree);
        auto const parent_miss_tree = make_warm(tree);
        auto const warm_uncached = make_warm(uncached);
        auto const warm_tiled = make_warm(tiled);
        auto const warm_instances = make_warm(instances_request);

        std::vector<Scenario> scenarios{
            {"builder_direct", [](std::size_t) { return builder_direct_once(); }, {}},
            {"tree_cold", cold(tree), {}},
            {"tree_warm",
             [&, tree](std::size_t) { return configure_once(*warm_tree, snapshot, tree); },
             cache_size(warm_tree)},
            {"tree_parent_miss",
             [&, tree](std::size_t repetition) {
                 auto request = tree;
                 // A unique root key; nested keys are unchanged and hit.
                 request.salt = 1'000'000 + static_cast<int>(repetition);
                 return configure_once(*parent_miss_tree, snapshot, request);
             },
             cache_size(parent_miss_tree)},
            {"tree_uncached",
             [&, uncached](std::size_t) {
                 return configure_once(*warm_uncached, snapshot, uncached);
             },
             cache_size(warm_uncached)},
            {"subgraphs_cold", cold(subgraphs), {}},
            {"tiled_cold", cold(tiled), {}},
            {"tiled_warm",
             [&, tiled](std::size_t) { return configure_once(*warm_tiled, snapshot, tiled); },
             cache_size(warm_tiled)},
            {"instances_warm",
             [&, instances_request](std::size_t) {
                 return configure_once(*warm_instances, snapshot, instances_request);
             },
             cache_size(warm_instances)},
        };

        std::cout << "shape depth=" << shape.depth
                  << " fanout=" << shape.fanout
                  << " leaves=" << shape.leaves
                  << " subgraphs=" << shape.subgraphs
                  << " instances=" << shape.instances
                  << " direct_nodes=" << shape.direct_nodes
                  << " padding_definitions=" << shape.padding_definitions
                  << " warmup=" << options.warmup << '\n';
        for (auto const& scenario : scenarios) {
            if (!options.scenarios.empty()
                && !std::ranges::contains(options.scenarios, scenario.name)) {
                continue;
            }
            report(scenario, options.warmup, options.repetitions);
        }
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "iv_graph_configuration_benchmark: " << error.what() << '\n';
        return 1;
    }
}
