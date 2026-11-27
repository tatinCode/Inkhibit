#include "inkhibit/album/rotation_store.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace album = inkhibit::album;
namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
using namespace std::chrono_literals;

#define REQUIRE(condition) do { if (!(condition)) { \
    throw std::runtime_error(std::string(__FILE__) + ":" \
        + std::to_string(__LINE__) + ": " + #condition); \
} } while (false)

template<class Exception, class Function>
void expect_throw(Function function)
{
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("Expected exception was not thrown");
}

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory()
    {
        auto pattern =
            (fs::current_path() / "rotation-store-tests-XXXXXX").string();
        const auto* result = ::mkdtemp(pattern.data());
        REQUIRE(result);
        path = result;
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    REQUIRE(out);
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    const std::string text{
        std::istreambuf_iterator<char>{in},
        std::istreambuf_iterator<char>{}};
    REQUIRE(!in.bad());
    return text;
}

void require_same(
    const album::RotationState& actual,
    const album::RotationState& expected)
{
    REQUIRE(actual.settings.interval == expected.settings.interval);
    REQUIRE(actual.settings.shuffle == expected.settings.shuffle);
    REQUIRE(actual.order == expected.order);
    REQUIRE(actual.remaining == expected.remaining);
    REQUIRE(actual.history == expected.history);
    REQUIRE(actual.history_position == expected.history_position);
    REQUIRE(actual.next_change == expected.next_change);
}

void require_loaded(const fs::path& root, const album::RotationState& expected)
{
    const auto loaded = album::load_rotation_state(root);
    REQUIRE(loaded);
    require_same(*loaded, expected);
}

album::RotationState example()
{
    album::RotationState state;
    state.settings.interval = 2h;
    state.settings.shuffle = true;
    state.order = {8, 2, 5};
    state.remaining = {5, 2};
    state.history = {99, 8, 8}; // Deleted IDs and repeated displays are valid.
    state.history_position = 2;
    state.next_change = Clock::time_point{100h};
    return state;
}

// Independent fixture: catches matching mistakes in the writer and reader.
std::string valid_text()
{
    return
        "INKHIBIT_ROTATION 1\n"
        "interval 7200\n"
        "shuffle 1\n"
        "deadline 1 360000\n"
        "position 1 2\n"
        "order 3 8 2 5\n"
        "remaining 2 5 2\n"
        "history 3 99 8 8\n";
}

std::string replace_once(
    std::string text, const std::string& from, const std::string& to)
{
    const auto position = text.find(from);
    REQUIRE(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}

void roundtrip()
{
    TemporaryDirectory directory;
    const auto state = example();
    const auto path = directory.path / ".rotation-state";

    // Verify reader and writer independently against the format fixture.
    write_file(path, valid_text());
    require_loaded(directory.path, state);
    album::save_rotation_state(directory.path, state);
    REQUIRE(read_file(path) == valid_text());
    require_loaded(directory.path, state);
    REQUIRE(!fs::exists(directory.path / ".rotation-state.tmp"));

    // Repeated load/save must not consume images or move the history cursor.
    const auto loaded = album::load_rotation_state(directory.path);
    REQUIRE(loaded);
    album::save_rotation_state(directory.path, *loaded);
    require_loaded(directory.path, state);
}

void optional_values()
{
    TemporaryDirectory directory;
    album::RotationState state;
    album::save_rotation_state(directory.path, state);
    require_loaded(directory.path, state);

    // Zero-valued optionals are present, not absent. IDs retain all 64 bits.
    state.order = {std::numeric_limits<album::ImageId>::max()};
    state.remaining = state.order;
    state.history = state.order;
    state.history_position = 0;
    state.next_change = Clock::time_point{};
    state.settings.interval = 30min;
    album::save_rotation_state(directory.path, state);
    require_loaded(directory.path, state);

    state.next_change = Clock::time_point{-1s};
    album::save_rotation_state(directory.path, state);
    require_loaded(directory.path, state);
}

void precision()
{
    TemporaryDirectory directory;
    for (const auto milliseconds : {1500ms, -1500ms}) {
        auto state = example();
        state.next_change = Clock::time_point{
            std::chrono::duration_cast<Clock::duration>(milliseconds)};
        album::save_rotation_state(directory.path, state);

        // The documented seconds format truncates toward zero.
        state.next_change = Clock::time_point{
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration_cast<std::chrono::seconds>(milliseconds))};
        require_loaded(directory.path, state);
    }
}

void missing()
{
    TemporaryDirectory directory;
    REQUIRE(!album::load_rotation_state(directory.path));
    REQUIRE(!fs::exists(directory.path / ".rotation-state"));

    const auto temporary = directory.path / ".rotation-state.tmp";
    write_file(temporary, valid_text());
    REQUIRE(!album::load_rotation_state(directory.path));
    REQUIRE(read_file(temporary) == valid_text());

    album::save_rotation_state(directory.path, example());
    write_file(temporary, "incomplete");
    require_loaded(directory.path, example());
    REQUIRE(read_file(temporary) == "incomplete");
}

void replacement()
{
    TemporaryDirectory directory;
    album::save_rotation_state(directory.path, example());

    // A shorter replacement must leave no bytes from the previous state.
    const album::RotationState replacement;
    album::save_rotation_state(directory.path, replacement);
    require_loaded(directory.path, replacement);
    REQUIRE(!fs::exists(directory.path / ".rotation-state.tmp"));

    // A stale, longer temporary file must be overwritten too.
    write_file(directory.path / ".rotation-state.tmp", std::string(4096, 'X'));
    album::save_rotation_state(directory.path, example());
    REQUIRE(read_file(directory.path / ".rotation-state") == valid_text());
}

void malformed_syntax()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    const auto count_max = std::to_string(
        std::numeric_limits<std::size_t>::max());
    const std::vector<std::pair<std::string, std::string>> changes{
        {"INKHIBIT_ROTATION 1", "WRONG 1"},
        {"INKHIBIT_ROTATION 1", "INKHIBIT_ROTATION 0"},
        {"INKHIBIT_ROTATION 1", "INKHIBIT_ROTATION 2"},
        {"interval 7200", "wrong 7200"},
        {"interval 7200", "interval 7200x"},
        {"interval 7200", "interval +7200"},
        {"interval 7200", "interval 999999999999999999999"},
        {"shuffle 1", "shuffle 2"},
        {"shuffle 1", "shuffle -1"},
        {"deadline 1 360000", "deadline 2 360000"},
        {"deadline 1 360000", "deadline 1 not-a-number"},
        {"deadline 1 360000", "deadline 1 999999999999999999999"},
        {"position 1 2", "position 2 2"},
        {"position 1 2", "position 1 -1"},
        {"position 1 2", "position 1 999999999999999999999"},
        {"order 3 8 2 5", "order -1 8 2 5"},
        {"order 3 8 2 5", "order 3 -8 2 5"},
        {"order 3 8 2 5", "order 3 18446744073709551616 2 5"},
        {"order 3 8 2 5", "order " + count_max + " 8 2 5"},
        {"remaining 2 5 2", "remaining " + count_max + " 5 2"},
        {"history 3 99 8 8", "history " + count_max + " 99 8 8"},
        {"history 3 99 8 8", "history 999999999999999999999 99 8 8"}
    };

    for (const auto& change : changes) {
        write_file(path, replace_once(valid_text(), change.first, change.second));
        expect_throw<std::runtime_error>([&] {
            album::load_rotation_state(directory.path);
        });
    }

    for (const auto* label : {
             "shuffle", "deadline", "position", "order", "remaining", "history"}) {
        write_file(path, replace_once(valid_text(), label, "wrong"));
        expect_throw<std::runtime_error>([&] {
            album::load_rotation_state(directory.path);
        });
    }

    write_file(path, std::string(100, 'X'));
    expect_throw<std::runtime_error>([&] {
        album::load_rotation_state(directory.path);
    });
    write_file(path, replace_once(valid_text(),
        "interval 7200", "interval " + std::string(64, '0')));
    expect_throw<std::runtime_error>([&] {
        album::load_rotation_state(directory.path);
    });
}

void malformed_state()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    const std::vector<std::pair<std::string, std::string>> changes{
        {"interval 7200", "interval 1799"},
        {"interval 7200", "interval -1"},
        {"position 1 2", "position 0"},
        {"position 1 2", "position 1 99"},
        {"order 3 8 2 5", "order 3 8 8 5"},
        {"order 3 8 2 5", "order 3 0 2 5"},
        {"remaining 2 5 2", "remaining 2 5 99"},
        {"remaining 2 5 2", "remaining 2 5 5"},
        {"history 3 99 8 8", "history 3 99 0 8"},
        {"history 3 99 8 8", "history 0"}
    };

    for (const auto& change : changes) {
        write_file(path, replace_once(valid_text(), change.first, change.second));
        expect_throw<std::invalid_argument>([&] {
            album::load_rotation_state(directory.path);
        });
    }
}

void truncated()
{
    TemporaryDirectory directory;
    const auto text = valid_text();
    const auto final_integer = text.find_last_of('8');
    REQUIRE(final_integer != std::string::npos);

    // Every byte prefix before the final required integer is incomplete.
    for (std::size_t length = 0; length <= final_integer; ++length) {
        write_file(directory.path / ".rotation-state", text.substr(0, length));
        expect_throw<std::runtime_error>([&] {
            album::load_rotation_state(directory.path);
        });
    }
}

void trailing_data()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    write_file(path, valid_text() + " \t\r\n");
    require_loaded(directory.path, example());

    for (const auto& suffix : {std::string("extra 1\n"), std::string(1, '\0')}) {
        write_file(path, valid_text() + suffix);
        expect_throw<std::runtime_error>([&] {
            album::load_rotation_state(directory.path);
        });
    }

    auto text = valid_text();
    text.pop_back(); // EOF immediately after the final integer is valid.
    write_file(path, text);
    require_loaded(directory.path, example());
}

void deadline_bounds()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    const auto minimum = std::chrono::duration_cast<std::chrono::seconds>(
        Clock::duration::min());
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(
        Clock::duration::max());

    for (const auto seconds : {minimum, maximum}) {
        write_file(path, replace_once(valid_text(), "deadline 1 360000",
            "deadline 1 " + std::to_string(seconds.count())));
        auto expected = example();
        expected.next_change = Clock::time_point{
            std::chrono::duration_cast<Clock::duration>(seconds)};
        require_loaded(directory.path, expected);
    }

    if (maximum < std::chrono::seconds::max()) {
        write_file(path, replace_once(valid_text(), "deadline 1 360000",
            "deadline 1 " + std::to_string(maximum.count() + 1)));
        expect_throw<std::runtime_error>([&] {
            album::load_rotation_state(directory.path);
        });
    }
    if (minimum > std::chrono::seconds::min()) {
        write_file(path, replace_once(valid_text(), "deadline 1 360000",
            "deadline 1 " + std::to_string(minimum.count() - 1)));
        expect_throw<std::runtime_error>([&] {
            album::load_rotation_state(directory.path);
        });
    }
}

void invalid_save()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    const auto temporary = directory.path / ".rotation-state.tmp";
    album::save_rotation_state(directory.path, example());
    const auto before = read_file(path);

    auto invalid = example();
    invalid.remaining = {999};
    expect_throw<std::invalid_argument>([&] {
        album::save_rotation_state(directory.path, invalid);
    });
    REQUIRE(read_file(path) == before);
    REQUIRE(!fs::exists(temporary));

    // Validation must also precede opening/truncating a stale temporary file.
    write_file(temporary, "leave this untouched");
    expect_throw<std::invalid_argument>([&] {
        album::save_rotation_state(directory.path, invalid);
    });
    REQUIRE(read_file(temporary) == "leave this untouched");
    REQUIRE(read_file(path) == before);
}

void open_failure()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    const auto temporary = directory.path / ".rotation-state.tmp";
    album::save_rotation_state(directory.path, example());
    const auto before = read_file(path);

    // Fails even as root, unlike permission-bit based tests.
    fs::create_directory(temporary);
    expect_throw<std::runtime_error>([&] {
        album::save_rotation_state(directory.path, album::RotationState{});
    });
    REQUIRE(read_file(path) == before);
    require_loaded(directory.path, example());

    fs::remove(temporary);
    album::save_rotation_state(directory.path, album::RotationState{});
    require_loaded(directory.path, album::RotationState{});
}

void write_failure()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    const auto temporary = directory.path / ".rotation-state.tmp";
    album::save_rotation_state(directory.path, example());
    const auto before = read_file(path);

    // Linux /dev/full opens successfully but refuses writes, including when
    // buffered output is flushed on close. The link is only in our test dir.
    REQUIRE(fs::exists("/dev/full"));
    fs::create_symlink("/dev/full", temporary);
    expect_throw<std::runtime_error>([&] {
        album::save_rotation_state(directory.path, album::RotationState{});
    });
    REQUIRE(fs::is_symlink(temporary));
    REQUIRE(read_file(path) == before);
    require_loaded(directory.path, example());

    fs::remove(temporary); // Remove only our symlink, never its target.
    album::save_rotation_state(directory.path, album::RotationState{});
    require_loaded(directory.path, album::RotationState{});
}

void rename_failure()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";
    fs::create_directory(path);
    write_file(path / "marker", "preserve this");

    expect_throw<fs::filesystem_error>([&] {
        album::save_rotation_state(directory.path, example());
    });
    REQUIRE(read_file(path / "marker") == "preserve this");
    REQUIRE(read_file(directory.path / ".rotation-state.tmp") == valid_text());

    fs::remove(path / "marker");
    fs::remove(path);
    album::save_rotation_state(directory.path, example());
    require_loaded(directory.path, example());
}

void invalid_path()
{
    TemporaryDirectory directory;
    const auto path = directory.path / ".rotation-state";

    fs::create_directory(path);
    expect_throw<std::runtime_error>([&] {
        album::load_rotation_state(directory.path);
    });
    fs::remove(path);

    // A status error must not be mistaken for a missing state file.
    fs::create_symlink(".rotation-state", path);
    expect_throw<fs::filesystem_error>([&] {
        album::load_rotation_state(directory.path);
    });
    fs::remove(path);

    // Save must not create the album directory itself.
    expect_throw<std::runtime_error>([&] {
        album::save_rotation_state(directory.path / "missing", example());
    });
    REQUIRE(!fs::exists(directory.path / "missing"));

    const auto not_directory = directory.path / "file";
    write_file(not_directory, "not a directory");
    expect_throw<fs::filesystem_error>([&] {
        album::load_rotation_state(not_directory);
    });
}

class GroupedNumbers : public std::numpunct<char> {
protected:
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

class ScopedLocale {
public:
    explicit ScopedLocale(const std::locale& locale)
        : previous_(std::locale::global(locale))
    {
    }
    ~ScopedLocale() { std::locale::global(previous_); }
    ScopedLocale(const ScopedLocale&) = delete;
    ScopedLocale& operator=(const ScopedLocale&) = delete;

private:
    std::locale previous_;
};

void locale()
{
    TemporaryDirectory directory;
    const ScopedLocale guard{
        std::locale{std::locale::classic(), new GroupedNumbers}};

    album::save_rotation_state(directory.path, example());
    REQUIRE(read_file(directory.path / ".rotation-state") == valid_text());
    require_loaded(directory.path, example());
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::pair<std::string, void (*)()>> cases{
            {"roundtrip", roundtrip},
            {"optional_values", optional_values},
            {"precision", precision},
            {"missing", missing},
            {"replacement", replacement},
            {"malformed_syntax", malformed_syntax},
            {"malformed_state", malformed_state},
            {"truncated", truncated},
            {"trailing_data", trailing_data},
            {"deadline_bounds", deadline_bounds},
            {"invalid_save", invalid_save},
            {"open_failure", open_failure},
            {"write_failure", write_failure},
            {"rename_failure", rename_failure},
            {"invalid_path", invalid_path},
            {"locale", locale}
        };
        if (argc != 2) {
            throw std::runtime_error("Expected one rotation store test case name");
        }
        for (const auto& entry : cases) {
            if (entry.first == argv[1]) {
                entry.second();
                return 0;
            }
        }
        throw std::runtime_error("Unknown rotation store test case");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
