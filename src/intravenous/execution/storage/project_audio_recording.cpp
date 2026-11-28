#include <intravenous/execution/storage/project_audio_recording.h>
#include <intravenous/wav.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace iv {
namespace {

constexpr std::size_t wave_header_size = 56;
constexpr std::uint64_t signed_index_bias = std::uint64_t{1} << 63;
constexpr auto write_interval = std::chrono::milliseconds(100);
constexpr std::size_t queue_capacity_bytes = 8 * 1024 * 1024;

bool is_within(std::filesystem::path const& root, std::filesystem::path const& path)
{
    auto const relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

void write_wave_header(std::ostream& file, std::uint32_t sample_rate,
    std::uint16_t channels, std::uint32_t frames)
{
    auto const bytes_per_frame = static_cast<std::uint16_t>(channels * sizeof(float));
    auto const data_bytes = frames * bytes_per_frame;
    using details::write_u16_le;
    using details::write_u32_le;
    file.write("RIFF", 4);
    write_u32_le(file, static_cast<std::uint32_t>(wave_header_size - 8) + data_bytes);
    file.write("WAVEfmt ", 8);
    write_u32_le(file, 16);
    write_u16_le(file, 3); // IEEE float WAV.
    write_u16_le(file, channels);
    write_u32_le(file, sample_rate);
    write_u32_le(file, sample_rate * bytes_per_frame);
    write_u16_le(file, bytes_per_frame);
    write_u16_le(file, 32);
    file.write("fact", 4);
    write_u32_le(file, 4);
    write_u32_le(file, frames);
    file.write("data", 4);
    write_u32_le(file, data_bytes);
}

void write_wave_samples(std::ostream& file, std::span<std::byte const> bytes)
{
    static_assert(sizeof(Sample) == sizeof(float) && sizeof(float) == sizeof(std::uint32_t));
    if constexpr (std::endian::native == std::endian::little) {
        file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    } else {
        for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(float)) {
            std::uint32_t bits;
            std::memcpy(&bits, bytes.data() + offset, sizeof(bits));
            details::write_u32_le(file, bits);
        }
    }
}

} // namespace

class ProjectAudioRecording::Impl {
    struct Part {
        std::filesystem::path path;
        std::uint32_t frames = 0;
    };

    struct Stream {
        Impl& owner;
        std::filesystem::path directory;
        std::uint32_t sample_rate;
        std::uint16_t channels;
        std::atomic<bool> failed{false};

        // After binding, these fields belong to the disk worker.
        std::filesystem::path take_directory{};
        std::map<std::uint64_t, Part> parts{};
        Part* open_part = nullptr;
        std::fstream file{};
        std::optional<std::uint64_t> next_frame{};
        bool dirty = false;

        Stream(Impl& owner_, std::filesystem::path directory_, std::uint32_t rate,
            std::uint16_t channel_count)
            : owner(owner_), directory(std::move(directory_)), sample_rate(rate), channels(channel_count) {}

        void fail(char const* reason) noexcept
        {
            if (!failed.exchange(true)) std::fprintf(stderr, "Sampler recording failed: %s\n", reason);
        }

        void checkpoint()
        {
            if (!dirty) return;
            file.seekp(0);
            write_wave_header(file, sample_rate, channels, open_part->frames);
            file.flush();
            next_frame.reset();
            dirty = false;
        }

        void open(Part& part)
        {
            if (open_part == &part) return;
            checkpoint();
            if (file.is_open()) file.close();
            file.exceptions(std::ios::badbit | std::ios::failbit);
            auto mode = std::ios::binary | std::ios::in | std::ios::out;
            if (part.frames == 0) mode |= std::ios::trunc;
            file.open(part.path, mode);
            open_part = &part;
            next_frame.reset();
        }

        Part& new_part(std::uint64_t key)
        {
            if (take_directory.empty()) take_directory = owner.create_take(directory);
            auto const begin = GlobalIndex::from_bits(key ^ signed_index_bias).signed_value();
            return parts.try_emplace(key, Part{take_directory / ("region-" + std::to_string(begin) + ".wav")}).first->second;
        }

        void write(GlobalIndex begin, std::span<std::byte const> bytes)
        {
            auto const stride = channels * sizeof(float);
            if (bytes.size() % stride != 0) throw std::runtime_error("unaligned Sample recording block");
            auto const maximum_frames = (std::numeric_limits<std::uint32_t>::max() - (wave_header_size - 8)) / stride;
            auto key = begin.bits() ^ signed_index_bias;
            while (!bytes.empty()) {
                auto next = parts.upper_bound(key);
                Part* part = nullptr;
                std::uint64_t offset = 0;
                if (next != parts.begin()) {
                    auto const previous = std::prev(next);
                    auto const distance = key - previous->first;
                    if (distance <= previous->second.frames && distance < maximum_frames) {
                        part = &previous->second;
                        offset = distance;
                    }
                }
                if (!part) part = &new_part(key);
                auto frames = std::min<std::uint64_t>(bytes.size() / stride, maximum_frames - offset);
                if (next != parts.end()) frames = std::min(frames, next->first - key);
                if (frames - 1 > std::numeric_limits<std::uint64_t>::max() - key)
                    frames = std::numeric_limits<std::uint64_t>::max() - key + 1;
                open(*part);
                if (next_frame != offset)
                    file.seekp(static_cast<std::streamoff>(wave_header_size + offset * stride));
                auto const length = frames * stride;
                write_wave_samples(file, bytes.first(length));
                part->frames = std::max(part->frames, static_cast<std::uint32_t>(offset + frames));
                next_frame = offset + frames;
                dirty = true;
                bytes = bytes.subspan(length);
                key += frames;
            }
        }
    };

    struct Block {
        Stream* stream;
        GlobalIndex begin;
        std::vector<std::byte> bytes;
    };

    std::filesystem::path root_;
    std::mutex mutex_{};
    std::condition_variable ready_{};
    std::map<std::tuple<std::uint64_t, std::filesystem::path, std::uint32_t, std::uint16_t>,
        std::unique_ptr<Stream>> streams_{};
    std::deque<Block> queued_{};
    std::size_t queued_bytes_ = 0;
    bool closing_ = false;
    std::uint64_t next_take_ = 0; // Disk worker only.
    std::jthread worker_;

    std::filesystem::path create_take(std::filesystem::path const& directory)
    {
        auto const resolved = std::filesystem::weakly_canonical(directory);
        if (!is_within(root_, resolved)) throw std::runtime_error("recording directory escapes the project");
        std::filesystem::create_directories(resolved);
        auto const date = std::chrono::system_clock::now().time_since_epoch().count();
        for (;;) {
            auto take = resolved / ("take-" + std::to_string(date) + "-" + std::to_string(next_take_++));
            if (std::filesystem::create_directory(take)) return take;
        }
    }

    void submit(Stream& stream, GlobalIndex begin, std::vector<std::byte>&& bytes) noexcept
    {
        if (bytes.empty() || stream.failed.load()) return;
        try {
            std::scoped_lock lock(mutex_);
            if (bytes.size() > queue_capacity_bytes - queued_bytes_) {
                stream.fail("disk writer fell behind");
                return;
            }
            auto const size = bytes.size();
            queued_.push_back({&stream, begin, std::move(bytes)});
            queued_bytes_ += size;
            ready_.notify_one();
        } catch (std::exception const& error) {
            stream.fail(error.what());
        }
    }

    void run()
    {
        for (;;) {
            std::deque<Block> batch;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return closing_ || !queued_.empty(); });
                if (queued_.empty()) return;
                ready_.wait_for(lock, write_interval, [&] { return closing_; });
                batch.swap(queued_);
            }
            std::size_t bytes = 0;
            std::vector<Stream*> touched;
            for (auto const& block : batch) {
                bytes += block.bytes.size();
                try {
                    if (!std::ranges::contains(touched, block.stream)) touched.push_back(block.stream);
                    block.stream->write(block.begin, block.bytes);
                } catch (std::exception const& error) {
                    block.stream->fail(error.what());
                }
            }
            for (auto* stream : touched) {
                try { stream->checkpoint(); }
                catch (std::exception const& error) { stream->fail(error.what()); }
            }
            batch.clear();
            std::scoped_lock lock(mutex_);
            queued_bytes_ -= bytes;
        }
    }

public:
    explicit Impl(std::filesystem::path root)
        : root_(std::filesystem::weakly_canonical(
              root.empty() ? std::filesystem::current_path() : std::filesystem::absolute(root))),
          worker_([this] { run(); }) {}

    ~Impl()
    {
        {
            std::scoped_lock lock(mutex_);
            closing_ = true;
        }
        ready_.notify_one();
        worker_.join();
    }

    SampleRecordingBinding bind(std::string_view directory, std::uint64_t output,
        std::size_t rate, std::size_t channels)
    {
        namespace fs = std::filesystem;
        fs::path const relative{directory};
        if (relative.empty() || relative.has_root_path() || directory.find('\0') != std::string_view::npos
            || std::ranges::contains(relative, fs::path{".."})) {
            throw std::invalid_argument("Sampler requires a project-relative recording directory");
        }
        auto const path = fs::weakly_canonical(root_ / relative);
        if (!is_within(root_, path)) throw std::invalid_argument("Sampler recording directory escapes the project");
        if (channels == 0 || channels > std::numeric_limits<std::uint16_t>::max() / sizeof(float)
            || rate == 0 || rate > std::numeric_limits<std::uint32_t>::max() / (channels * sizeof(float))) {
            throw std::invalid_argument("Sampler output cannot be represented as a WAV file");
        }
        auto const wav_rate = static_cast<std::uint32_t>(rate);
        auto const wav_channels = static_cast<std::uint16_t>(channels);
        std::scoped_lock lock(mutex_);
        auto const key = std::tuple{output, path, wav_rate, wav_channels};
        auto found = streams_.find(key);
        if (found == streams_.end()) {
            found = streams_.emplace(key, std::make_unique<Stream>(*this, path, wav_rate, wav_channels)).first;
        }
        return {found->second.get(), +[](void* target, GlobalIndex begin, std::vector<std::byte>&& bytes) noexcept {
            auto& stream = *static_cast<Stream*>(target);
            stream.owner.submit(stream, begin, std::move(bytes));
        }};
    }
};

ProjectAudioRecording::ProjectAudioRecording(std::filesystem::path root)
    : impl_(std::make_unique<Impl>(std::move(root))) {}

ProjectAudioRecording::~ProjectAudioRecording() = default;

ResourceContext::SampleRecordingResources ProjectAudioRecording::resources() noexcept
{
    return {impl_.get(), +[](void* owner, std::string_view directory, std::uint64_t output,
                std::size_t rate, std::size_t channels) {
        return static_cast<Impl*>(owner)->bind(directory, output, rate, channels);
    }};
}

} // namespace iv
