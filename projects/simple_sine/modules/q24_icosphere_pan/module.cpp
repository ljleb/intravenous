#include <intravenous/dsl.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

using namespace iv;

struct LearnedHrtfSourceIcosphere
{
    static constexpr size_t latent_parameter_count = 24;
    static constexpr size_t pinna_filter_count = 8;
    static constexpr size_t input_history = 63;

    static constexpr float speed_of_sound = 343.0f;
    static constexpr float head_radius = 0.0875f;

    static constexpr char coefficient_path[] = "/home/ljleb/Downloads/P0014_icosphere_frequency_local_runtime.bin";

    // The learned hierarchy contains detail coefficients through Loop level 4.
    // Runtime evaluation then performs four additional *local* Loop refinements
    // before the final barycentric sample (a level-8 approximation to the limit).
    static constexpr size_t learned_level = 4;
    static constexpr size_t lookup_level = 6;
    static constexpr size_t limit_refinement_steps = 4;
    static constexpr size_t loop_level_count = lookup_level + 1;

    static constexpr std::array<size_t, loop_level_count> vertex_count_by_level {
        12, 42, 162, 642, 2562, 10242, 40962,
    };

    static constexpr std::array<size_t, loop_level_count> face_count_by_level {
        20, 80, 320, 1280, 5120, 20480, 81920,
    };

    static constexpr std::array<size_t, loop_level_count> vertex_offset_by_level {
        0,
        12,
        12 + 42,
        12 + 42 + 162,
        12 + 42 + 162 + 642,
        12 + 42 + 162 + 642 + 2562,
        12 + 42 + 162 + 642 + 2562 + 10242,
    };

    static constexpr std::array<size_t, loop_level_count> face_offset_by_level {
        0,
        20,
        20 + 80,
        20 + 80 + 320,
        20 + 80 + 320 + 1280,
        20 + 80 + 320 + 1280 + 5120,
        20 + 80 + 320 + 1280 + 5120 + 20480,
    };

    static constexpr size_t total_geometry_vertices =
        12 + 42 + 162 + 642 + 2562 + 10242 + 40962;

    static constexpr size_t total_geometry_faces =
        20 + 80 + 320 + 1280 + 5120 + 20480 + 81920;

    static constexpr size_t adjacency_face_count =
        20 + 80 + 320 + 1280 + 5120 + 20480;

    static constexpr size_t learned_vertex_count = 2562;
    static constexpr size_t learned_face_count = 5120;

    // A radius-five face neighbourhood at level 4 contains ~46 faces on this
    // mesh. One local subdivision temporarily expands it to < 200 faces.
    static constexpr size_t patch_capacity = 256;
    static constexpr size_t patch_face_radius = 5;
    static constexpr uint16_t invalid_index = 0xffffu;

    static constexpr auto inputs()
    {
        return std::array<InputConfig, 3> {
            InputConfig { .name = "in", .history = input_history, },
            InputConfig { .name = "azimuth", .default_value = 0, .min = -180, .max = 180, },
            InputConfig { .name = "elevation", .default_value = 0, .min = -180, .max = 180, },
        };
    }

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1> {
            OutputConfig {
                .name = "out",
                .channel_layout = { .channel_type = ChannelTypeId::stereo, },
            },
        };
    }

    struct Vec3
    {
        float x = 0;
        float y = 0;
        float z = 0;
    };

    struct Barycentric
    {
        float a = 0;
        float b = 0;
        float c = 0;
    };

    struct Face
    {
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t c = 0;
    };

    struct LatentVector
    {
        std::array<float, latent_parameter_count> value {};
    };

    struct BiquadCoefficients
    {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
    };

    struct BiquadState
    {
        float s1 = 0.0f;
        float s2 = 0.0f;
    };

    struct HeadCoefficients
    {
        float b0 = 1.0f;
        float b1 = 0.0f;
    };

    struct HeadState
    {
        float previous_input = 0.0f;
        float previous_output = 0.0f;
    };

    struct EarParameters
    {
        std::array<float, 4> peak_frequency {};
        std::array<float, 4> notch_frequency {};
        std::array<float, 4> peak_gain {};
        std::array<float, 4> notch_gain {};
        std::array<float, 4> peak_q {};
        std::array<float, 4> notch_q {};
    };

    struct EarRuntime
    {
        HeadCoefficients head {};
        HeadState head_state {};
        std::array<BiquadCoefficients, pinna_filter_count> filters {};
        std::array<BiquadState, pinna_filter_count> filter_state {};
        float delay_samples = 0;
    };

    struct Patch
    {
        size_t vertex_count = 0;
        size_t face_count = 0;
        size_t center_face = 0;
        std::array<LatentVector, patch_capacity> control {};
        std::array<Face, patch_capacity> face {};
    };

    struct State
    {
        // Geometry/topology is generated once in initialize(). Keeping it in
        // node state deliberately trades memory for direct, cache-local reads.
        std::array<Vec3, total_geometry_vertices> geometry_vertex {};
        std::array<Face, total_geometry_faces> geometry_face {};
        std::array<float, total_geometry_faces> face_inverse_centroid_length {};

        // Adjacency is needed only through level 5. Level-6 lookup tests the
        // final candidate triangles directly.
        std::array<std::array<uint16_t, 3>, adjacency_face_count> face_neighbor {};

        // Sparse hierarchical coefficients are expanded to this contiguous
        // level-4 control mesh at initialization. Each vertex carries all 24
        // independent latent scalar fields.
        std::array<LatentVector, learned_vertex_count> control {};

        Patch patch_a {};
        Patch patch_b {};

        float azimuth = 0.0f;
        float elevation = 0.0f;
        float sample_rate = 48000.0f;

        float head_omega0 = 0.0f;
        float head_inverse_denominator = 0.0f;
        float head_feedback = 0.0f;
        float radius_over_c_samples = 0.0f;

        std::array<EarRuntime, 2> ear {};
    };

    void initialize(InitializationContext<LearnedHrtfSourceIcosphere> const& ctx) const
    {
        auto& state = ctx.state();

        initialize_loop_geometry(state);
        initialize_face_acceleration(state);
        load_and_expand_coefficients(state);

        state.head_omega0 = speed_of_sound / head_radius;
        state.head_inverse_denominator =
            1.0f / (state.head_omega0 + state.sample_rate);
        state.head_feedback =
            (state.sample_rate - state.head_omega0)
            * state.head_inverse_denominator;
        state.radius_over_c_samples =
            head_radius * state.sample_rate / speed_of_sound;

        update_direction(state, 0.0f, 0.0f);
    }

    void tick_block(TickBlockContext<LearnedHrtfSourceIcosphere> const& ctx) const
    {
        auto& state = ctx.state();
        auto const& input = ctx.inputs[0];
        auto const azimuth = ctx.input<"azimuth">();
        auto const elevation = ctx.input<"elevation">();
        auto output = ctx.output<"out">();

        for (size_t i = 0; i < ctx.block_size; ++i) {
            auto const current_azimuth = static_cast<float>(azimuth[i]);
            auto const current_elevation = static_cast<float>(elevation[i]);

            if (current_azimuth != state.azimuth || current_elevation != state.elevation)
                update_direction(state, current_azimuth, current_elevation);

            for (size_t channel = 0; channel < 2; ++channel) {
                auto& ear = state.ear[channel];

                auto value = read_fractional_delay(input, i, ear.delay_samples);
                value = process_head(ear, state.head_feedback, value);

                for (size_t filter = 0; filter < pinna_filter_count; ++filter)
                    value = process_biquad(
                        ear.filters[filter],
                        ear.filter_state[filter],
                        value);

                if (channel == 0)
                    output[stereo::left][i] = value;
                else
                    output[stereo::right][i] = value;
            }
        }
    }

private:
    // ---------- small vector helpers ----------

    static Vec3 add(Vec3 a, Vec3 b)
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }

    static Vec3 subtract(Vec3 a, Vec3 b)
    {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    static Vec3 multiply(Vec3 a, float s)
    {
        return { a.x * s, a.y * s, a.z * s };
    }

    static float dot(Vec3 a, Vec3 b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vec3 cross(Vec3 a, Vec3 b)
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        };
    }

    static float minimum_component(Barycentric const& b)
    {
        return std::min(b.a, std::min(b.b, b.c));
    }

    // ---------- model file ----------

    static uint32_t read_u32(std::ifstream& file)
    {
        uint32_t value = 0;
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        if (!file)
            throw std::runtime_error("truncated icosphere model file");
        return value;
    }

    static void read_level_terms(
        std::ifstream& file,
        size_t expected_level,
        std::vector<LatentVector>& values)
    {
        auto const expected_vertices = vertex_count_by_level[expected_level];
        auto const file_vertices = read_u32(file);
        auto const term_count = read_u32(file);

        if (file_vertices != expected_vertices)
            throw std::runtime_error("icosphere model vertex count mismatch");

        for (uint32_t n = 0; n < term_count; ++n) {
            uint16_t vertex = 0;
            uint8_t field = 0;
            uint8_t reserved = 0;
            float coefficient = 0;

            file.read(reinterpret_cast<char*>(&vertex), sizeof(vertex));
            file.read(reinterpret_cast<char*>(&field), sizeof(field));
            file.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));
            file.read(reinterpret_cast<char*>(&coefficient), sizeof(coefficient));

            if (!file)
                throw std::runtime_error("truncated icosphere model term");
            if (vertex >= values.size() || field >= latent_parameter_count)
                throw std::runtime_error("invalid icosphere model term");

            values[vertex].value[field] += coefficient;
        }
    }

    static void load_and_expand_coefficients(State& state)
    {
        std::ifstream file(coefficient_path, std::ios::binary);
        if (!file)
            throw std::runtime_error(std::string("could not open ") + coefficient_path);

        std::array<char, 8> magic {};
        file.read(magic.data(), magic.size());

        constexpr std::array<char, 8> expected_magic {
            'I','V','I','C','O','2','4','\0'
        };

        if (magic != expected_magic)
            throw std::runtime_error("invalid icosphere model magic");

        auto const version = read_u32(file);
        auto const max_level = read_u32(file);
        auto const field_count = read_u32(file);
        (void)read_u32(file); // total sparse scalar control count; informational.

        if (version != 1 || max_level != learned_level || field_count != latent_parameter_count)
            throw std::runtime_error("unsupported icosphere model format");

        std::vector<LatentVector> current(vertex_count_by_level[0]);
        read_level_terms(file, 0, current);

        for (size_t level = 0; level < learned_level; ++level) {
            std::vector<LatentVector> next(vertex_count_by_level[level + 1]);
            refine_dense_control_level(state, level, current, next);
            read_level_terms(file, level + 1, next);
            current.swap(next);
        }

        for (size_t i = 0; i < learned_vertex_count; ++i)
            state.control[i] = current[i];
    }

    // ---------- Loop geometry construction ----------

    struct EdgeSeed
    {
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t opposite = 0;
    };

    struct EdgeInfo
    {
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t opposite0 = 0;
        uint16_t opposite1 = 0;
    };

    static constexpr std::array<Face, 20> root_faces {{
        {6,2,10}, {7,10,0}, {7,8,6}, {7,6,10}, {3,4,2},
        {3,6,8}, {3,2,6}, {11,0,10}, {11,10,2}, {11,2,4},
        {11,4,5}, {11,5,0}, {1,0,5}, {1,7,0}, {1,8,7},
        {9,3,8}, {9,5,4}, {9,4,3}, {9,1,5}, {9,8,1},
    }};

    static void add_neighbor(
        std::array<uint16_t, 6>& neighbors,
        uint8_t& count,
        uint16_t value)
    {
        for (uint8_t i = 0; i < count; ++i)
            if (neighbors[i] == value)
                return;

        neighbors[count++] = value;
    }

    static std::vector<EdgeInfo> build_edges(
        State const& state,
        size_t level,
        std::vector<std::array<uint16_t, 6>>& neighbors,
        std::vector<uint8_t>& neighbor_count)
    {
        auto const face_count = face_count_by_level[level];
        auto const face_offset = face_offset_by_level[level];

        std::vector<EdgeSeed> seed;
        seed.reserve(face_count * 3);

        auto append = [&](uint16_t i, uint16_t j, uint16_t opposite) {
            if (i > j)
                std::swap(i, j);
            seed.push_back({ i, j, opposite });
        };

        for (size_t i = 0; i < face_count; ++i) {
            auto const f = state.geometry_face[face_offset + i];

            add_neighbor(neighbors[f.a], neighbor_count[f.a], f.b);
            add_neighbor(neighbors[f.a], neighbor_count[f.a], f.c);
            add_neighbor(neighbors[f.b], neighbor_count[f.b], f.a);
            add_neighbor(neighbors[f.b], neighbor_count[f.b], f.c);
            add_neighbor(neighbors[f.c], neighbor_count[f.c], f.a);
            add_neighbor(neighbors[f.c], neighbor_count[f.c], f.b);

            append(f.a, f.b, f.c);
            append(f.b, f.c, f.a);
            append(f.c, f.a, f.b);
        }

        std::sort(seed.begin(), seed.end(), [](EdgeSeed const& a, EdgeSeed const& b) {
            if (a.a != b.a)
                return a.a < b.a;
            if (a.b != b.b)
                return a.b < b.b;
            return a.opposite < b.opposite;
        });

        std::vector<EdgeInfo> edge;
        edge.reserve(seed.size() / 2);

        for (size_t i = 0; i < seed.size();) {
            auto const a = seed[i].a;
            auto const b = seed[i].b;
            auto const opposite0 = seed[i].opposite;
            auto const opposite1 = seed[i + 1].opposite;
            edge.push_back({ a, b, opposite0, opposite1 });
            i += 2;
        }

        return edge;
    }

    static size_t find_edge(
        std::vector<EdgeInfo> const& edge,
        uint16_t a,
        uint16_t b)
    {
        if (a > b)
            std::swap(a, b);

        auto const it = std::lower_bound(
            edge.begin(),
            edge.end(),
            std::pair<uint16_t, uint16_t> { a, b },
            [](EdgeInfo const& e, std::pair<uint16_t, uint16_t> key) {
                if (e.a != key.first)
                    return e.a < key.first;
                return e.b < key.second;
            });

        return static_cast<size_t>(it - edge.begin());
    }

    static float loop_beta(size_t valence)
    {
        auto const n = static_cast<float>(valence);
        auto const t =
            3.0f / 8.0f
            + 0.25f * std::cos(
                2.0f * std::numbers::pi_v<float> / n);

        return (5.0f / 8.0f - t * t) / n;
    }

    static void initialize_root_geometry(State& state)
    {
        auto const phi = (1.0f + std::sqrt(5.0f)) * 0.5f;
        auto const inverse_length = 1.0f / std::sqrt(1.0f + phi * phi);

        constexpr std::array<Vec3, 12> raw {{
            {-1, +1.618033988749895f, 0}, {+1, +1.618033988749895f, 0},
            {-1, -1.618033988749895f, 0}, {+1, -1.618033988749895f, 0},
            {0, -1, +1.618033988749895f}, {0, +1, +1.618033988749895f},
            {0, -1, -1.618033988749895f}, {0, +1, -1.618033988749895f},
            {+1.618033988749895f, 0, -1}, {+1.618033988749895f, 0, +1},
            {-1.618033988749895f, 0, -1}, {-1.618033988749895f, 0, +1},
        }};

        auto const vertex_offset = vertex_offset_by_level[0];
        for (size_t i = 0; i < raw.size(); ++i)
            state.geometry_vertex[vertex_offset + i] = multiply(raw[i], inverse_length);

        auto const face_offset = face_offset_by_level[0];
        for (size_t i = 0; i < root_faces.size(); ++i)
            state.geometry_face[face_offset + i] = root_faces[i];
    }

    static void build_next_geometry_level(State& state, size_t level)
    {
        auto const vertex_count = vertex_count_by_level[level];
        auto const vertex_offset = vertex_offset_by_level[level];
        auto const next_vertex_offset = vertex_offset_by_level[level + 1];
        auto const face_count = face_count_by_level[level];
        auto const face_offset = face_offset_by_level[level];
        auto const next_face_offset = face_offset_by_level[level + 1];

        std::vector<std::array<uint16_t, 6>> neighbors(vertex_count);
        std::vector<uint8_t> neighbor_count(vertex_count, 0);
        auto const edge = build_edges(state, level, neighbors, neighbor_count);

        for (size_t i = 0; i < vertex_count; ++i) {
            auto const beta = loop_beta(neighbor_count[i]);
            auto value = multiply(
                state.geometry_vertex[vertex_offset + i],
                1.0f - beta * neighbor_count[i]);

            for (uint8_t n = 0; n < neighbor_count[i]; ++n)
                value = add(
                    value,
                    multiply(
                        state.geometry_vertex[vertex_offset + neighbors[i][n]],
                        beta));

            state.geometry_vertex[next_vertex_offset + i] = value;
        }

        for (size_t i = 0; i < edge.size(); ++i) {
            auto const& e = edge[i];
            auto value = add(
                multiply(
                    add(
                        state.geometry_vertex[vertex_offset + e.a],
                        state.geometry_vertex[vertex_offset + e.b]),
                    3.0f / 8.0f),
                multiply(
                    add(
                        state.geometry_vertex[vertex_offset + e.opposite0],
                        state.geometry_vertex[vertex_offset + e.opposite1]),
                    1.0f / 8.0f));

            state.geometry_vertex[next_vertex_offset + vertex_count + i] = value;
        }

        for (size_t i = 0; i < face_count; ++i) {
            auto const f = state.geometry_face[face_offset + i];
            auto const ab = static_cast<uint16_t>(
                vertex_count + find_edge(edge, f.a, f.b));
            auto const bc = static_cast<uint16_t>(
                vertex_count + find_edge(edge, f.b, f.c));
            auto const ca = static_cast<uint16_t>(
                vertex_count + find_edge(edge, f.c, f.a));

            auto const out = next_face_offset + i * 4;
            state.geometry_face[out + 0] = { f.a, ab, ca };
            state.geometry_face[out + 1] = { f.b, bc, ab };
            state.geometry_face[out + 2] = { f.c, ca, bc };
            state.geometry_face[out + 3] = { ab, bc, ca };
        }
    }

    static void initialize_loop_geometry(State& state)
    {
        initialize_root_geometry(state);
        for (size_t level = 0; level < lookup_level; ++level)
            build_next_geometry_level(state, level);
    }

    static void refine_dense_control_level(
        State const& state,
        size_t level,
        std::vector<LatentVector> const& current,
        std::vector<LatentVector>& next)
    {
        auto const vertex_count = vertex_count_by_level[level];

        std::vector<std::array<uint16_t, 6>> neighbors(vertex_count);
        std::vector<uint8_t> neighbor_count(vertex_count, 0);
        auto const edge = build_edges(state, level, neighbors, neighbor_count);

        for (size_t i = 0; i < vertex_count; ++i) {
            auto const beta = loop_beta(neighbor_count[i]);
            auto const center_weight = 1.0f - beta * neighbor_count[i];

            for (size_t k = 0; k < latent_parameter_count; ++k) {
                float value = center_weight * current[i].value[k];
                for (uint8_t n = 0; n < neighbor_count[i]; ++n)
                    value += beta * current[neighbors[i][n]].value[k];
                next[i].value[k] = value;
            }
        }

        for (size_t i = 0; i < edge.size(); ++i) {
            auto const& e = edge[i];
            auto& out = next[vertex_count + i];

            for (size_t k = 0; k < latent_parameter_count; ++k) {
                out.value[k] =
                    3.0f / 8.0f
                        * (current[e.a].value[k] + current[e.b].value[k])
                    + 1.0f / 8.0f
                        * (current[e.opposite0].value[k] + current[e.opposite1].value[k]);
            }
        }
    }

    // ---------- face lookup acceleration ----------

    struct FaceEdgeSeed
    {
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t face = 0;
        uint8_t slot = 0;
    };

    static void build_face_adjacency(State& state, size_t level)
    {
        auto const count = face_count_by_level[level];
        auto const offset = face_offset_by_level[level];

        for (size_t i = 0; i < count; ++i)
            state.face_neighbor[offset + i] = {
                invalid_index,
                invalid_index,
                invalid_index,
            };

        std::vector<FaceEdgeSeed> edge;
        edge.reserve(count * 3);

        auto append = [&](uint16_t a, uint16_t b, uint16_t face, uint8_t slot) {
            if (a > b)
                std::swap(a, b);
            edge.push_back({ a, b, face, slot });
        };

        for (uint16_t i = 0; i < count; ++i) {
            auto const f = state.geometry_face[offset + i];
            append(f.a, f.b, i, 0);
            append(f.b, f.c, i, 1);
            append(f.c, f.a, i, 2);
        }

        std::sort(edge.begin(), edge.end(), [](FaceEdgeSeed const& a, FaceEdgeSeed const& b) {
            if (a.a != b.a)
                return a.a < b.a;
            if (a.b != b.b)
                return a.b < b.b;
            return a.face < b.face;
        });

        for (size_t i = 0; i < edge.size(); i += 2) {
            auto const& a = edge[i];
            auto const& b = edge[i + 1];
            state.face_neighbor[offset + a.face][a.slot] = b.face;
            state.face_neighbor[offset + b.face][b.slot] = a.face;
        }
    }

    static void initialize_face_acceleration(State& state)
    {
        for (size_t level = 0; level < loop_level_count; ++level) {
            auto const face_count = face_count_by_level[level];
            auto const face_offset = face_offset_by_level[level];
            auto const vertex_offset = vertex_offset_by_level[level];

            for (size_t i = 0; i < face_count; ++i) {
                auto const f = state.geometry_face[face_offset + i];
                auto const centroid = add(
                    add(
                        state.geometry_vertex[vertex_offset + f.a],
                        state.geometry_vertex[vertex_offset + f.b]),
                    state.geometry_vertex[vertex_offset + f.c]);

                state.face_inverse_centroid_length[face_offset + i] =
                    1.0f / std::sqrt(dot(centroid, centroid));
            }
        }

        for (size_t level = 0; level < lookup_level; ++level)
            build_face_adjacency(state, level);
    }

    static float face_score(
        State const& state,
        size_t level,
        uint32_t face,
        Vec3 direction)
    {
        auto const face_offset = face_offset_by_level[level];
        auto const vertex_offset = vertex_offset_by_level[level];
        auto const f = state.geometry_face[face_offset + face];

        auto const centroid = add(
            add(
                state.geometry_vertex[vertex_offset + f.a],
                state.geometry_vertex[vertex_offset + f.b]),
            state.geometry_vertex[vertex_offset + f.c]);

        return
            dot(centroid, direction)
            * state.face_inverse_centroid_length[face_offset + face];
    }

    static Barycentric ray_barycentric(
        State const& state,
        size_t level,
        uint32_t face,
        Vec3 direction)
    {
        auto const f = state.geometry_face[face_offset_by_level[level] + face];
        auto const vertex_offset = vertex_offset_by_level[level];
        auto const a = state.geometry_vertex[vertex_offset + f.a];
        auto const b = state.geometry_vertex[vertex_offset + f.b];
        auto const c = state.geometry_vertex[vertex_offset + f.c];

        auto const ab = subtract(b, a);
        auto const ac = subtract(c, a);
        auto const normal = cross(ab, ac);
        auto const scale = dot(normal, a) / dot(normal, direction);
        auto const q = multiply(direction, scale);
        auto const aq = subtract(q, a);

        auto const d00 = dot(ab, ab);
        auto const d01 = dot(ab, ac);
        auto const d11 = dot(ac, ac);
        auto const d20 = dot(aq, ab);
        auto const d21 = dot(aq, ac);
        auto const inverse = 1.0f / (d00 * d11 - d01 * d01);

        auto const v = (d11 * d20 - d01 * d21) * inverse;
        auto const w = (d00 * d21 - d01 * d20) * inverse;
        return { 1.0f - v - w, v, w };
    }

    static void add_unique_face(
        std::array<uint16_t, 4>& list,
        size_t& count,
        uint16_t value)
    {
        if (value == invalid_index)
            return;
        for (size_t i = 0; i < count; ++i)
            if (list[i] == value)
                return;
        list[count++] = value;
    }

    static uint32_t choose_centroid_child(
        State const& state,
        size_t level,
        uint32_t parent,
        Vec3 direction)
    {
        auto const previous_offset = face_offset_by_level[level - 1];
        std::array<uint16_t, 4> parents {};
        size_t parent_count = 0;

        add_unique_face(parents, parent_count, static_cast<uint16_t>(parent));
        for (auto n : state.face_neighbor[previous_offset + parent])
            add_unique_face(parents, parent_count, n);

        float best_score = -std::numeric_limits<float>::infinity();
        uint32_t best_face = 0;

        for (size_t p = 0; p < parent_count; ++p) {
            auto const first_child = static_cast<uint32_t>(parents[p]) * 4;
            for (uint32_t child = 0; child < 4; ++child) {
                auto const face = first_child + child;
                auto const score = face_score(state, level, face, direction);
                if (score > best_score) {
                    best_score = score;
                    best_face = face;
                }
            }
        }

        return best_face;
    }

    static Barycentric parent_barycentric(uint32_t child, Barycentric b)
    {
        switch (child) {
        case 0:
            return {
                b.a + 0.5f * (b.b + b.c),
                0.5f * b.b,
                0.5f * b.c,
            };
        case 1:
            return {
                0.5f * b.c,
                b.a + 0.5f * (b.b + b.c),
                0.5f * b.b,
            };
        case 2:
            return {
                0.5f * b.b,
                0.5f * b.c,
                b.a + 0.5f * (b.b + b.c),
            };
        default:
            return {
                0.5f * (b.a + b.c),
                0.5f * (b.a + b.b),
                0.5f * (b.b + b.c),
            };
        }
    }

    struct ParameterLocation
    {
        uint16_t face4 = 0;
        Barycentric bary4 {};
    };

    static ParameterLocation locate_parameter(
        State const& state,
        Vec3 direction)
    {
        uint32_t face = 0;
        float best_score = -std::numeric_limits<float>::infinity();

        for (uint32_t i = 0; i < face_count_by_level[0]; ++i) {
            auto const score = face_score(state, 0, i, direction);
            if (score > best_score) {
                best_score = score;
                face = i;
            }
        }

        for (size_t level = 1; level < lookup_level; ++level)
            face = choose_centroid_child(state, level, face, direction);

        // At level 6 evaluate the same small child candidate set directly.
        auto const previous_offset = face_offset_by_level[lookup_level - 1];
        std::array<uint16_t, 4> parents {};
        size_t parent_count = 0;
        add_unique_face(parents, parent_count, static_cast<uint16_t>(face));
        for (auto n : state.face_neighbor[previous_offset + face])
            add_unique_face(parents, parent_count, n);

        bool found_inside = false;
        float best_inside_score = -std::numeric_limits<float>::infinity();
        float best_minimum = -std::numeric_limits<float>::infinity();
        uint32_t face6 = 0;
        Barycentric bary6 {};

        for (size_t p = 0; p < parent_count; ++p) {
            auto const first_child = static_cast<uint32_t>(parents[p]) * 4;
            for (uint32_t child = 0; child < 4; ++child) {
                auto const candidate = first_child + child;
                auto const bary = ray_barycentric(
                    state,
                    lookup_level,
                    candidate,
                    direction);
                auto const minimum = minimum_component(bary);
                auto const score = face_score(
                    state,
                    lookup_level,
                    candidate,
                    direction);

                if (minimum >= -2.0e-4f) {
                    if (!found_inside || score > best_inside_score) {
                        found_inside = true;
                        best_inside_score = score;
                        face6 = candidate;
                        bary6 = bary;
                    }
                }
                else if (!found_inside && minimum > best_minimum) {
                    best_minimum = minimum;
                    face6 = candidate;
                    bary6 = bary;
                }
            }
        }

        auto const face5 = face6 / 4;
        auto const child6 = face6 % 4;
        auto const face4 = face5 / 4;
        auto const child5 = face5 % 4;

        auto bary5 = parent_barycentric(child6, bary6);
        auto bary4 = parent_barycentric(child5, bary5);

        return {
            static_cast<uint16_t>(face4),
            bary4,
        };
    }

    // ---------- local Loop evaluation ----------

    static size_t patch_find_vertex(
        std::array<uint16_t, patch_capacity> const& global_vertex,
        size_t count,
        uint16_t vertex)
    {
        for (size_t i = 0; i < count; ++i)
            if (global_vertex[i] == vertex)
                return i;
        return count;
    }

    static void extract_level4_patch(
        State const& state,
        uint16_t center_face,
        Patch& patch)
    {
        std::bitset<learned_face_count> visited;
        std::array<uint16_t, patch_capacity> queue {};
        std::array<uint8_t, patch_capacity> depth {};
        std::array<uint16_t, patch_capacity> selected {};
        size_t queue_begin = 0;
        size_t queue_end = 0;
        size_t selected_count = 0;

        queue[queue_end] = center_face;
        depth[queue_end] = 0;
        ++queue_end;
        visited.set(center_face);

        auto const adjacency_offset = face_offset_by_level[learned_level];

        while (queue_begin < queue_end) {
            auto const face = queue[queue_begin];
            auto const d = depth[queue_begin];
            ++queue_begin;

            selected[selected_count++] = face;

            if (d == patch_face_radius)
                continue;

            for (auto n : state.face_neighbor[adjacency_offset + face]) {
                if (n != invalid_index && !visited.test(n)) {
                    visited.set(n);
                    queue[queue_end] = n;
                    depth[queue_end] = static_cast<uint8_t>(d + 1);
                    ++queue_end;
                }
            }
        }

        patch.vertex_count = 0;
        patch.face_count = 0;
        patch.center_face = 0;

        std::array<uint16_t, patch_capacity> global_vertex {};
        auto const face_offset = face_offset_by_level[learned_level];

        for (size_t s = 0; s < selected_count; ++s) {
            auto const global_face = selected[s];
            auto const f = state.geometry_face[face_offset + global_face];
            std::array<uint16_t, 3> gv { f.a, f.b, f.c };
            std::array<uint16_t, 3> lv {};

            for (size_t j = 0; j < 3; ++j) {
                auto local = patch_find_vertex(
                    global_vertex,
                    patch.vertex_count,
                    gv[j]);

                if (local == patch.vertex_count) {
                    global_vertex[patch.vertex_count] = gv[j];
                    patch.control[patch.vertex_count] = state.control[gv[j]];
                    ++patch.vertex_count;
                }

                lv[j] = static_cast<uint16_t>(local);
            }

            patch.face[patch.face_count] = { lv[0], lv[1], lv[2] };
            if (global_face == center_face)
                patch.center_face = patch.face_count;
            ++patch.face_count;
        }
    }

    struct PatchEdge
    {
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t opposite0 = 0;
        uint16_t opposite1 = 0;
        uint8_t opposite_count = 0;
    };

    static size_t find_patch_edge(
        std::array<PatchEdge, patch_capacity> const& edge,
        size_t edge_count,
        uint16_t a,
        uint16_t b)
    {
        if (a > b)
            std::swap(a, b);

        for (size_t i = 0; i < edge_count; ++i)
            if (edge[i].a == a && edge[i].b == b)
                return i;
        return edge_count;
    }

    static void add_patch_neighbor(
        std::array<uint16_t, 6>& neighbor,
        uint8_t& count,
        uint16_t value)
    {
        for (uint8_t i = 0; i < count; ++i)
            if (neighbor[i] == value)
                return;
        neighbor[count++] = value;
    }

    static void add_patch_edge(
        std::array<PatchEdge, patch_capacity>& edge,
        size_t& edge_count,
        uint16_t a,
        uint16_t b,
        uint16_t opposite)
    {
        if (a > b)
            std::swap(a, b);

        auto index = find_patch_edge(edge, edge_count, a, b);
        if (index == edge_count) {
            edge[index] = { a, b, opposite, 0, 1 };
            ++edge_count;
        }
        else if (edge[index].opposite_count == 1) {
            edge[index].opposite1 = opposite;
            edge[index].opposite_count = 2;
        }
    }

    static void refine_patch(Patch const& source, Patch& destination)
    {
        std::array<std::array<uint16_t, 6>, patch_capacity> neighbor {};
        std::array<uint8_t, patch_capacity> neighbor_count {};
        std::array<PatchEdge, patch_capacity> edge {};
        size_t edge_count = 0;

        for (size_t i = 0; i < source.face_count; ++i) {
            auto const f = source.face[i];

            add_patch_neighbor(neighbor[f.a], neighbor_count[f.a], f.b);
            add_patch_neighbor(neighbor[f.a], neighbor_count[f.a], f.c);
            add_patch_neighbor(neighbor[f.b], neighbor_count[f.b], f.a);
            add_patch_neighbor(neighbor[f.b], neighbor_count[f.b], f.c);
            add_patch_neighbor(neighbor[f.c], neighbor_count[f.c], f.a);
            add_patch_neighbor(neighbor[f.c], neighbor_count[f.c], f.b);

            add_patch_edge(edge, edge_count, f.a, f.b, f.c);
            add_patch_edge(edge, edge_count, f.b, f.c, f.a);
            add_patch_edge(edge, edge_count, f.c, f.a, f.b);
        }

        destination.vertex_count = source.vertex_count + edge_count;
        destination.face_count = source.face_count * 4;

        for (size_t i = 0; i < source.vertex_count; ++i) {
            auto const count = neighbor_count[i];
            auto const beta = loop_beta(count);
            auto const center_weight = 1.0f - beta * count;

            for (size_t k = 0; k < latent_parameter_count; ++k) {
                float value = center_weight * source.control[i].value[k];
                for (uint8_t n = 0; n < count; ++n)
                    value += beta * source.control[neighbor[i][n]].value[k];
                destination.control[i].value[k] = value;
            }
        }

        for (size_t i = 0; i < edge_count; ++i) {
            auto const& e = edge[i];
            auto& out = destination.control[source.vertex_count + i];

            for (size_t k = 0; k < latent_parameter_count; ++k) {
                if (e.opposite_count == 2) {
                    out.value[k] =
                        3.0f / 8.0f
                            * (source.control[e.a].value[k]
                               + source.control[e.b].value[k])
                        + 1.0f / 8.0f
                            * (source.control[e.opposite0].value[k]
                               + source.control[e.opposite1].value[k]);
                }
                else {
                    // Patch boundary only. The queried face is five rings away,
                    // so this fallback never propagates into the sampled value.
                    out.value[k] =
                        0.5f
                        * (source.control[e.a].value[k]
                           + source.control[e.b].value[k]);
                }
            }
        }

        for (size_t i = 0; i < source.face_count; ++i) {
            auto const f = source.face[i];
            auto const ab = static_cast<uint16_t>(
                source.vertex_count + find_patch_edge(edge, edge_count, f.a, f.b));
            auto const bc = static_cast<uint16_t>(
                source.vertex_count + find_patch_edge(edge, edge_count, f.b, f.c));
            auto const ca = static_cast<uint16_t>(
                source.vertex_count + find_patch_edge(edge, edge_count, f.c, f.a));

            auto const out = i * 4;
            destination.face[out + 0] = { f.a, ab, ca };
            destination.face[out + 1] = { f.b, bc, ab };
            destination.face[out + 2] = { f.c, ca, bc };
            destination.face[out + 3] = { ab, bc, ca };
        }
    }

    static size_t shared_vertex_count(Face a, Face b)
    {
        std::array<uint16_t, 3> av { a.a, a.b, a.c };
        std::array<uint16_t, 3> bv { b.a, b.b, b.c };
        size_t count = 0;
        for (auto x : av)
            for (auto y : bv)
                count += x == y;
        return count;
    }

    static void crop_patch(
        Patch const& source,
        size_t center_face,
        Patch& destination)
    {
        std::array<std::array<uint16_t, 3>, patch_capacity> adjacency {};
        std::array<uint8_t, patch_capacity> adjacency_count {};

        for (size_t i = 0; i < source.face_count; ++i)
            for (size_t j = i + 1; j < source.face_count; ++j)
                if (shared_vertex_count(source.face[i], source.face[j]) == 2) {
                    adjacency[i][adjacency_count[i]++] = static_cast<uint16_t>(j);
                    adjacency[j][adjacency_count[j]++] = static_cast<uint16_t>(i);
                }

        std::array<uint8_t, patch_capacity> visited {};
        std::array<uint16_t, patch_capacity> queue {};
        std::array<uint8_t, patch_capacity> depth {};
        std::array<uint16_t, patch_capacity> selected {};
        size_t queue_begin = 0;
        size_t queue_end = 0;
        size_t selected_count = 0;

        queue[queue_end] = static_cast<uint16_t>(center_face);
        depth[queue_end] = 0;
        ++queue_end;
        visited[center_face] = 1;

        while (queue_begin < queue_end) {
            auto const face = queue[queue_begin];
            auto const d = depth[queue_begin];
            ++queue_begin;

            selected[selected_count++] = face;

            if (d == patch_face_radius)
                continue;

            for (uint8_t n = 0; n < adjacency_count[face]; ++n) {
                auto const next = adjacency[face][n];
                if (!visited[next]) {
                    visited[next] = 1;
                    queue[queue_end] = next;
                    depth[queue_end] = static_cast<uint8_t>(d + 1);
                    ++queue_end;
                }
            }
        }

        destination.vertex_count = 0;
        destination.face_count = 0;
        destination.center_face = 0;

        std::array<int16_t, patch_capacity> vertex_map {};
        vertex_map.fill(-1);

        for (size_t s = 0; s < selected_count; ++s) {
            auto const source_face = selected[s];
            auto const f = source.face[source_face];
            std::array<uint16_t, 3> old { f.a, f.b, f.c };
            std::array<uint16_t, 3> mapped {};

            for (size_t j = 0; j < 3; ++j) {
                if (vertex_map[old[j]] < 0) {
                    vertex_map[old[j]] = static_cast<int16_t>(destination.vertex_count);
                    destination.control[destination.vertex_count] = source.control[old[j]];
                    ++destination.vertex_count;
                }
                mapped[j] = static_cast<uint16_t>(vertex_map[old[j]]);
            }

            destination.face[destination.face_count] = {
                mapped[0], mapped[1], mapped[2]
            };

            if (source_face == center_face)
                destination.center_face = destination.face_count;

            ++destination.face_count;
        }
    }

    static uint32_t choose_parameter_child(Barycentric& bary)
    {
        if (bary.a >= 0.5f) {
            bary = {
                2.0f * bary.a - 1.0f,
                2.0f * bary.b,
                2.0f * bary.c,
            };
            return 0;
        }

        if (bary.b >= 0.5f) {
            bary = {
                2.0f * bary.b - 1.0f,
                2.0f * bary.c,
                2.0f * bary.a,
            };
            return 1;
        }

        if (bary.c >= 0.5f) {
            bary = {
                2.0f * bary.c - 1.0f,
                2.0f * bary.a,
                2.0f * bary.b,
            };
            return 2;
        }

        auto const a = bary.a;
        auto const b = bary.b;
        auto const c = bary.c;
        bary = {
            a + b - c,
            b + c - a,
            c + a - b,
        };
        return 3;
    }

    static std::array<float, latent_parameter_count>
    evaluate_latent(State& state, Vec3 direction)
    {
        auto const location = locate_parameter(state, direction);
        auto bary = location.bary4;

        extract_level4_patch(state, location.face4, state.patch_a);

        for (size_t level = 0; level < limit_refinement_steps; ++level) {
            refine_patch(state.patch_a, state.patch_b);

            auto const child = choose_parameter_child(bary);
            auto const child_face = state.patch_a.center_face * 4 + child;

            if (level + 1 == limit_refinement_steps) {
                auto const f = state.patch_b.face[child_face];
                std::array<float, latent_parameter_count> result {};

                for (size_t k = 0; k < latent_parameter_count; ++k) {
                    result[k] =
                        bary.a * state.patch_b.control[f.a].value[k]
                        + bary.b * state.patch_b.control[f.b].value[k]
                        + bary.c * state.patch_b.control[f.c].value[k];
                }

                return result;
            }

            crop_patch(state.patch_b, child_face, state.patch_a);
        }

        return {};
    }

    // ---------- existing HRTF decoder / DSP ----------

    static Sample read_ago(InputPort const& input, size_t frame, size_t samples_ago)
    {
        if (samples_ago <= frame)
            return input.get_frame(frame - samples_ago);
        return input.get(samples_ago - frame);
    }

    static Sample read_fractional_delay(InputPort const& input, size_t frame, float delay)
    {
        auto const whole = static_cast<size_t>(delay);
        auto const fraction = delay - static_cast<float>(whole);
        auto const a = read_ago(input, frame, whole);
        auto const b = read_ago(input, frame, whole + 1);
        return a + fraction * (b - a);
    }

    static float softplus(float x)
    {
        return std::log1p(std::exp(x));
    }

    static float sigmoid(float x)
    {
        return 1.0f / (1.0f + std::exp(-x));
    }

    static void decode_frequency_group(
        float const* latent,
        std::array<float, 4>& frequency)
    {
        constexpr float minimum_frequency = 1500.0f;
        constexpr float maximum_frequency = 22000.0f;
        auto const log_frequency_range =
            std::log(maximum_frequency / minimum_frequency);

        std::array<float, 4> gap {
            softplus(latent[0]),
            softplus(latent[1]),
            softplus(latent[2]),
            softplus(latent[3]),
        };

        auto const inverse_total =
            1.0f / (1.0f + gap[0] + gap[1] + gap[2] + gap[3]);

        float cumulative = 0.0f;
        for (size_t i = 0; i < 4; ++i) {
            cumulative += gap[i];
            frequency[i] =
                minimum_frequency
                * std::exp(log_frequency_range * cumulative * inverse_total);
        }
    }

    static EarParameters decode(
        std::array<float, latent_parameter_count> const& latent)
    {
        constexpr float maximum_gain_db = 30.0f;
        constexpr float minimum_q = 0.3f;
        constexpr float q_range = 20.0f - minimum_q;

        EarParameters result;
        decode_frequency_group(latent.data(), result.peak_frequency);
        decode_frequency_group(latent.data() + 4, result.notch_frequency);

        for (size_t i = 0; i < 4; ++i) {
            result.peak_gain[i] = maximum_gain_db * sigmoid(latent[8 + i]);
            result.notch_gain[i] = -maximum_gain_db * sigmoid(latent[12 + i]);
            result.peak_q[i] = minimum_q + q_range * sigmoid(latent[16 + i]);
            result.notch_q[i] = minimum_q + q_range * sigmoid(latent[20 + i]);
        }

        return result;
    }

    static BiquadCoefficients peaking_eq(
        float frequency,
        float gain_db,
        float q,
        float sample_rate)
    {
        auto const A =
            std::exp(gain_db * std::numbers::ln10_v<float> / 40.0f);
        auto const omega =
            2.0f * std::numbers::pi_v<float> * frequency / sample_rate;
        auto const sin_omega = std::sin(omega);
        auto const cos_omega = std::cos(omega);
        auto const alpha = sin_omega / (2.0f * q);
        auto const b0 = 1.0f + alpha * A;
        auto const b1 = -2.0f * cos_omega;
        auto const b2 = 1.0f - alpha * A;
        auto const a0 = 1.0f + alpha / A;
        auto const a1 = -2.0f * cos_omega;
        auto const a2 = 1.0f - alpha / A;
        auto const inverse_a0 = 1.0f / a0;

        return {
            b0 * inverse_a0,
            b1 * inverse_a0,
            b2 * inverse_a0,
            a1 * inverse_a0,
            a2 * inverse_a0,
        };
    }

    static float process_biquad(
        BiquadCoefficients const& coefficients,
        BiquadState& state,
        float input)
    {
        auto const output = coefficients.b0 * input + state.s1;
        state.s1 =
            coefficients.b1 * input
            - coefficients.a1 * output
            + state.s2;
        state.s2 = coefficients.b2 * input - coefficients.a2 * output;
        return output;
    }

    static float process_head(
        EarRuntime& ear,
        float feedback,
        float input)
    {
        auto& state = ear.head_state;
        auto const output =
            ear.head.b0 * input
            + ear.head.b1 * state.previous_input
            + feedback * state.previous_output;

        state.previous_input = input;
        state.previous_output = output;
        return output;
    }

    static void update_ear(
        State& state,
        size_t channel,
        float x,
        float y,
        float z)
    {
        auto const latent = evaluate_latent(state, { x, y, z });
        auto const parameters = decode(latent);
        auto& ear = state.ear[channel];

        constexpr float alpha_min = 0.1f;
        constexpr float theta_min =
            150.0f * std::numbers::pi_v<float> / 180.0f;

        auto const theta = std::acos(y);
        auto const head_theta = std::min(theta, theta_min);
        auto const alpha =
            (1.0f + alpha_min * 0.5f)
            + (1.0f - alpha_min * 0.5f)
                * std::cos(
                    std::numbers::pi_v<float>
                    * head_theta / theta_min);

        ear.head.b0 =
            (state.head_omega0 + alpha * state.sample_rate)
            * state.head_inverse_denominator;
        ear.head.b1 =
            (state.head_omega0 - alpha * state.sample_rate)
            * state.head_inverse_denominator;

        constexpr float half_pi = std::numbers::pi_v<float> / 2.0f;
        float relative_delay;

        if (theta < half_pi)
            relative_delay = -state.radius_over_c_samples * y;
        else
            relative_delay =
                state.radius_over_c_samples * (theta - half_pi);

        ear.delay_samples = state.radius_over_c_samples + relative_delay;

        for (size_t i = 0; i < 4; ++i) {
            ear.filters[i * 2] = peaking_eq(
                parameters.peak_frequency[i],
                parameters.peak_gain[i],
                parameters.peak_q[i],
                state.sample_rate);

            ear.filters[i * 2 + 1] = peaking_eq(
                parameters.notch_frequency[i],
                parameters.notch_gain[i],
                parameters.notch_q[i],
                state.sample_rate);
        }
    }

    static void update_direction(State& state, float azimuth, float elevation)
    {
        auto const azimuth_radians =
            azimuth * std::numbers::pi_v<float> / 180.0f;
        auto const elevation_radians =
            elevation * std::numbers::pi_v<float> / 180.0f;
        auto const cos_elevation = std::cos(elevation_radians);
        auto const x = cos_elevation * std::cos(azimuth_radians);
        auto const y = -cos_elevation * std::sin(azimuth_radians);
        auto const z = std::sin(elevation_radians);

        update_ear(state, 0, x, y, z);
        update_ear(state, 1, x, -y, z);

        state.azimuth = azimuth;
        state.elevation = elevation;
    }
};

struct BaselineFir256
{
    static constexpr size_t tap_count = 256;
    static constexpr size_t input_history = tap_count - 1;

    static constexpr char coefficient_path[] =
        "/home/ljleb/Downloads/P0014_v2_runtime_baseline_ir_256.csv";

    static constexpr auto inputs()
    {
        return std::array<InputConfig, 1> {
            InputConfig {
                .name = "in",
                .channel_layout = { .channel_type = ChannelTypeId::stereo, },
                .history = input_history,
            },
        };
    }

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1> {
            OutputConfig {
                .name = "out",
                .channel_layout = { .channel_type = ChannelTypeId::stereo, },
            },
        };
    }

    struct State
    {
        std::array<float, tap_count> taps {};
    };

    void initialize(InitializationContext<BaselineFir256> const& ctx) const
    {
        auto& state = ctx.state();

        std::ifstream file(coefficient_path);

        if (!file)
            throw std::runtime_error(std::string("could not open ") + coefficient_path);

        std::string line;
        std::getline(file, line);

        for (size_t i = 0; i < tap_count; ++i) {
            if (!std::getline(file, line))
                throw std::runtime_error("incomplete baseline FIR CSV");

            auto const comma = line.rfind(',');
            if (comma == std::string::npos)
                throw std::runtime_error("malformed baseline FIR CSV");

            char* end = nullptr;
            state.taps[i] = std::strtof(line.c_str() + comma + 1, &end);

            if (end == line.c_str() + comma + 1)
                throw std::runtime_error("malformed baseline FIR coefficient");
        }
    }

    void tick_block(TickBlockContext<BaselineFir256> const& ctx) const
    {
        auto const& state = ctx.state();
        auto const& input = ctx.inputs[0];
        auto output = ctx.output<"out">();

        for (size_t frame = 0; frame < ctx.block_size; ++frame) {
            auto const current_taps = std::min(frame + 1, tap_count);

            for (size_t channel = 0; channel < 2; ++channel) {
                float sum = 0.0f;

                for (size_t tap = 0; tap < current_taps; ++tap)
                    sum += state.taps[tap] * input.get_frame(frame - tap, channel);

                for (size_t tap = current_taps; tap < tap_count; ++tap)
                    sum += state.taps[tap] * input.get(tap - frame, channel);

                if (channel == 0)
                    output[stereo::left][frame] = sum;
                else
                    output[stereo::right][frame] = sum;
            }
        }
    }
};

consteval void single_pan(GraphBuilder& g)
{
    auto const in = g.input<"in">();
    auto const az = g.input<"azimuth">(0, -180, 180);
    auto const el = g.input<"elevation">(0, -180, 180);

    auto const hrtf = g.node<LearnedHrtfSourceIcosphere>();

    hrtf(
        "in"_P = in,
        "azimuth"_P = az,
        "elevation"_P = el
    );

    g.outputs(hrtf);
}

consteval void module_main(GraphBuilder& g)
{
    auto const in = TypedSamplePortRef<stereo>{ static_cast<SamplePortRef>(g.input<"in", stereo>()) };
    auto const center = g.input<"azimuth">(0, -180, 180);
    auto const el = g.input<"elevation">(0, -180, 180);
    auto const spread = g.input<"spread">(30, 0, 180);
    auto const az = g.tile<stereo>(center - spread * 0.5, center + spread * 0.5);
    auto const fir = g.node<BaselineFir256>();

    auto const v_l = g.module<single_pan>();
    auto const v_r = g.module<single_pan>();

    auto const v_l_out = TypedSamplePortRef<stereo>{v_l(
        "in"_P = in[stereo::left],
        "azimuth"_P = az[stereo::left],
        "elevation"_P = el
    )};

    auto const v_r_out = TypedSamplePortRef<stereo>{v_r(
        "in"_P = in[stereo::right],
        "azimuth"_P = az[stereo::right],
        "elevation"_P = el
    )};

    fir(v_l_out + v_r_out);

    g.outputs("main"_P = fir);
}
