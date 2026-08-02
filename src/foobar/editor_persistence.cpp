#ifdef _WIN32

#include "editor_persistence.hpp"

#include "loop_finder/loop_engine.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace loop_finder::foobar {

namespace {

constexpr GUID kEditorIndexGuid = {
    0xb8e4a275,
    0x9af6,
    0x4fc9,
    {0xa8, 0x72, 0x39, 0x70, 0x84, 0xc4, 0xc1, 0x1e}};
constexpr std::uint32_t kRecordMagic = 0x4445464cU; // "LFED"
constexpr std::uint32_t kRecordSchema = 1;
constexpr t_filetimestamp kRetentionPeriod = system_time_periods::week * 26;

bool g_index_available = false;

class EditorIndexClient : public metadb_index_client {
public:
    metadb_index_hash transform(const file_info&,
                                const playable_location& location) override {
        pfc::string_formatter identity;
        identity << location.get_path() << "\n" << location.get_subsong_index();
        return hasher_md5::get()->process_single_string(identity).xorHalve();
    }
};

EditorIndexClient* index_client() {
    static auto* client =
        new service_impl_single_t<EditorIndexClient>();
    return client;
}

metadb_index_manager* index_manager() {
    static metadb_index_manager* manager =
        metadb_index_manager::get().detach();
    return manager;
}

class EditorIndexInitializer : public init_stage_callback {
public:
    void on_init_stage(t_uint32 stage) override {
        if (stage != init_stages::before_config_read) {
            return;
        }
        try {
            index_manager()->add(index_client(),
                                 kEditorIndexGuid,
                                 kRetentionPeriod);
            g_index_available = true;
        } catch (const std::exception& exception) {
            index_manager()->remove(kEditorIndexGuid);
            FB2K_console_formatter()
                << "[foo_loop_finder] Editor persistence unavailable: "
                << exception;
        }
    }
};

service_factory_single_t<EditorIndexInitializer> g_editor_index_initializer;

metadb_index_hash track_hash(const metadb_handle_ptr& track) {
    if (track.is_empty()) {
        throw std::invalid_argument("No track is available for editor metadata");
    }
    file_info_impl unused;
    return index_client()->transform(unused, track->get_location());
}

template <typename Integer>
void append_integer(std::vector<std::uint8_t>& output, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        output.push_back(static_cast<std::uint8_t>(bits & 0xffU));
        bits >>= 8U;
    }
}

void append_double(std::vector<std::uint8_t>& output, double value) {
    append_integer(output, std::bit_cast<std::uint64_t>(value));
}

template <typename Integer>
bool read_integer(std::span<const std::uint8_t> input,
                  std::size_t& cursor,
                  Integer& value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    if (input.size() - (std::min)(cursor, input.size()) < sizeof(Unsigned)) {
        return false;
    }
    Unsigned bits = 0;
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        bits |= static_cast<Unsigned>(input[cursor++]) << (index * 8U);
    }
    value = static_cast<Integer>(bits);
    return true;
}

bool read_double(std::span<const std::uint8_t> input,
                 std::size_t& cursor,
                 double& value) {
    std::uint64_t bits = 0;
    if (!read_integer(input, cursor, bits)) {
        return false;
    }
    value = std::bit_cast<double>(bits);
    return true;
}

std::vector<std::uint8_t> encode(const LoopState& state) {
    std::vector<std::uint8_t> output;
    output.reserve(52);
    append_integer(output, kRecordMagic);
    append_integer(output, kRecordSchema);
    append_double(output, state.bpm);
    append_double(output, state.grid_offset_seconds);
    append_integer(output, state.beats_per_bar);
    append_integer(output,
                   static_cast<std::uint32_t>(snap_mode(state)));
    append_double(output, state.in_seconds);
    append_double(output, state.out_seconds);
    return output;
}

std::optional<LoopState> decode(std::span<const std::uint8_t> input) {
    std::size_t cursor = 0;
    std::uint32_t magic = 0;
    std::uint32_t schema = 0;
    double bpm = 0.0;
    double offset = 0.0;
    std::uint32_t beats = 0;
    std::uint32_t snapping = 0;
    double in_seconds = 0.0;
    double out_seconds = 0.0;
    if (!read_integer(input, cursor, magic) || magic != kRecordMagic ||
        !read_integer(input, cursor, schema) || schema > kRecordSchema ||
        !read_double(input, cursor, bpm) ||
        !read_double(input, cursor, offset) ||
        !read_integer(input, cursor, beats) ||
        !read_integer(input, cursor, snapping) ||
        !read_double(input, cursor, in_seconds) ||
        !read_double(input, cursor, out_seconds)) {
        return std::nullopt;
    }

    // Schema 0 stored the same numeric subdivision field and implied snapping
    // on. Schema 1 makes zero an explicit free-placement mode.
    if (schema == 0 && snapping == 0) {
        snapping = 1;
    }

    LoopEngine restored;
    if (!restored.set_bpm(bpm).valid ||
        !restored.set_grid_offset(offset).valid ||
        !restored.set_beats_per_bar(beats).valid ||
        !restored.set_snapping(static_cast<SnapMode>(snapping)).valid ||
        !restored.set_markers(in_seconds, out_seconds).valid) {
        return std::nullopt;
    }
    return restored.state();
}

} // namespace

std::optional<LoopState> EditorPersistence::load(
    const metadb_handle_ptr& track,
    std::string& warning) {
    core_api::ensure_main_thread();
    warning.clear();
    if (!g_index_available || track.is_empty()) {
        return std::nullopt;
    }
    try {
        mem_block_container_impl data;
        index_manager()->get_user_data(kEditorIndexGuid,
                                       track_hash(track),
                                       data);
        if (data.get_size() == 0) {
            return std::nullopt;
        }
        const auto bytes = std::span(
            static_cast<const std::uint8_t*>(data.get_ptr()),
            static_cast<std::size_t>(data.get_size()));
        auto restored = decode(bytes);
        if (!restored.has_value()) {
            warning = "Saved editor data was invalid; defaults restored";
        }
        return restored;
    } catch (const std::exception& exception) {
        warning = std::string("Could not load editor data: ") + exception.what();
        return std::nullopt;
    }
}

bool EditorPersistence::save(const metadb_handle_ptr& track,
                             const LoopState& state,
                             std::string& error) {
    core_api::ensure_main_thread();
    error.clear();
    if (!g_index_available || track.is_empty()) {
        error = "Editor persistence is unavailable";
        return false;
    }
    try {
        LoopEngine validated;
        if (!validated.set_bpm(state.bpm).valid ||
            !validated.set_grid_offset(state.grid_offset_seconds).valid ||
            !validated.set_beats_per_bar(state.beats_per_bar).valid ||
            !validated.set_snapping(snap_mode(state)).valid ||
            !validated.set_markers(state.in_seconds, state.out_seconds).valid) {
            error = "Editor state did not pass LoopEngine validation";
            return false;
        }
        const auto data = encode(validated.state());
        index_manager()->set_user_data(kEditorIndexGuid,
                                       track_hash(track),
                                       data.data(),
                                       data.size());
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Could not save editor data: ") + exception.what();
        return false;
    }
}

} // namespace loop_finder::foobar

#endif
