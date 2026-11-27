#include "inkhibit/album/rotation_store.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <charconv>
#include <iomanip>
#include <locale>
#include <system_error>


namespace inkhibit::album {

    namespace {

        void write_ids(std::ostream& out, const char* name,
                const std::vector<ImageId>& ids){
            out << name << ' ' << ids.size();

            for (const auto id : ids) {
                out << ' ' << id;
            }

            out << '\n';
        }

        std::string read_token(std::istream& in){
            std::string token;

            // All valid labels and numeric values fit comfortably below this limit.
            if (!(in >> std::setw(64) >> token) || token.size() >= 64) {
                throw std::runtime_error("Invalid or incomplete rotation state");
            }

            return token;
        }

        void expect_label(std::istream& in, const char* expected){
            if (read_token(in) != expected) {
                throw std::runtime_error(
                        std::string("Expected rotation state field: ") + expected);
            }
        }

        template<class Integer>
            Integer read_integer(std::istream& in){
                const auto token = read_token(in);
                Integer value{};

                const auto result = std::from_chars(
                        token.data(), token.data() + token.size(), value);

                if (result.ec != std::errc{}
                        || result.ptr != token.data() + token.size()) {
                    throw std::runtime_error("Invalid rotation state integer");
                }

                return value;
            }

        bool read_flag(std::istream& in){
            const auto value = read_integer<unsigned>(in);

            if (value > 1) {
                throw std::runtime_error("Rotation state flag must be 0 or 1");
            }

            return value == 1;
        }

        std::vector<ImageId> read_ids(std::istream& in, const char* label){
            expect_label(in, label);
            const auto count = read_integer<std::size_t>(in);

            std::vector<ImageId> ids;

            // Do not reserve based on an untrusted count. A truncated file should
            // fail while reading, rather than request an enormous allocation.
            for (std::size_t i = 0; i < count; ++i) {
                ids.push_back(read_integer<ImageId>(in));
            }

            return ids;
        }

    } // end of namespace

    void save_rotation_state(
            const std::filesystem::path& album_root,
            const RotationState& state){
        validate_rotation_state(state);

        const auto destination = album_root / ".rotation-state";
        const auto temporary = album_root / ".rotation-state.tmp";

        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            throw std::runtime_error(
                    "Could not open temporary rotation state: "
                    + temporary.string());
        }

        out.imbue(std::locale::classic());

        // Versioned text format. Timestamps use Unix seconds.
        out << "INKHIBIT_ROTATION 1\n";
        out << "interval " << state.settings.interval.count() << '\n';
        out << "shuffle " << (state.settings.shuffle ? 1 : 0) << '\n';

        out << "deadline ";
        if (state.next_change) {
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(
                        state.next_change->time_since_epoch());

            out << "1 " << seconds.count() << '\n';
        } else {
            out << "0\n";
        }

        out << "position ";
        if (state.history_position) {
            out << "1 " << *state.history_position << '\n';
        } else {
            out << "0\n";
        }

        write_ids(out, "order", state.order);
        write_ids(out, "remaining", state.remaining);
        write_ids(out, "history", state.history);

        out.close();
        if (!out) {
            throw std::runtime_error(
                    "Could not write rotation state: " + temporary.string());
        }

        std::filesystem::rename(temporary, destination);

    }   //end of rotation_state

    std::optional<RotationState> load_rotation_state(
            const std::filesystem::path& album_root)
    {
        namespace fs = std::filesystem;

        const auto path = album_root / ".rotation-state";

        std::error_code error;
        const auto status = fs::status(path, error);

        if (error == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }

        if (error) {
            throw fs::filesystem_error(
                    "Could not inspect rotation state", path, error);
        }

        if (!fs::exists(status)) {
            return std::nullopt;
        }

        if (!fs::is_regular_file(status)) {
            throw std::runtime_error(
                    "Rotation state is not a regular file: " + path.string());
        }

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error(
                    "Could not open rotation state: " + path.string());
        }

        in.imbue(std::locale::classic());

        expect_label(in, "INKHIBIT_ROTATION");
        if (read_integer<unsigned>(in) != 1) {
            throw std::runtime_error("Unsupported rotation state version");
        }

        RotationState state;

        expect_label(in, "interval");
        state.settings.interval = std::chrono::seconds{
            read_integer<std::chrono::seconds::rep>(in)};

        expect_label(in, "shuffle");
        state.settings.shuffle = read_flag(in);

        expect_label(in, "deadline");
        if (read_flag(in)) {
            using Clock = std::chrono::system_clock;

            const auto seconds = std::chrono::seconds{
                read_integer<std::chrono::seconds::rep>(in)};

            // Check before converting seconds to the clock's finer duration.
            const auto minimum =
                std::chrono::duration_cast<std::chrono::seconds>(
                        Clock::duration::min());
            const auto maximum =
                std::chrono::duration_cast<std::chrono::seconds>(
                        Clock::duration::max());

            if (seconds < minimum || seconds > maximum) {
                throw std::runtime_error("Rotation deadline is out of range");
            }

            state.next_change = Clock::time_point{
                std::chrono::duration_cast<Clock::duration>(seconds)};
        }

        expect_label(in, "position");
        if (read_flag(in)) {
            state.history_position = read_integer<std::size_t>(in);
        }

        state.order = read_ids(in, "order");
        state.remaining = read_ids(in, "remaining");
        state.history = read_ids(in, "history");

        // Allow trailing whitespace, but reject extra fields or garbage.
        in >> std::ws;
        if (in.bad() || !in.eof()) {
            throw std::runtime_error(
                    "Unexpected trailing data or read failure in rotation state");
        }

        validate_rotation_state(state);
        return state;
    }

} // namespace inkhibit::album
