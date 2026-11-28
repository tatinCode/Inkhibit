#include "inkhibit/album/rotation_display.hpp"
#include "inkhibit/image.hpp"
#include "album_png_fixture.hpp"
#include "fake_display.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace album = inkhibit::album;
namespace fs = std::filesystem;
using Result = album::RotationDisplayResult;
using Outcome = fake_display::Outcome;
using Steady = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;
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
            (fs::current_path() / "rotation-display-tests-XXXXXX").string();
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

void write_bytes(const fs::path& path, const Bytes& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out);
}

// Independently computes the frame the adapter should send.
Bytes expected_frame(const fs::path& path)
{
    namespace img = inkhibit::image;
    return img::quantize_to_framebuffer(
        img::fit_to_canvas(img::decode_image(path)),
        img::Dither::floyd_steinberg);
}

// Sets stop a delay after `started` becomes true, or after a safety timeout.
// Always joins, even if a REQUIRE fails.
class DelayedStop {
public:
    DelayedStop(std::atomic<bool>& stop, const std::atomic<bool>& started,
            Steady::duration delay)
        : thread_([&stop, &started, delay] {
            const auto limit = Steady::now() + 10s;
            while (!started && Steady::now() < limit) {
                std::this_thread::sleep_for(1ms);
            }
            std::this_thread::sleep_for(delay);
            stop = true;
        })
    {
    }

    ~DelayedStop() { thread_.join(); }

    DelayedStop(const DelayedStop&) = delete;
    DelayedStop& operator=(const DelayedStop&) = delete;

private:
    std::thread thread_;
};

const fs::path default_state_directory = "/var/lib/inkhibit";

void success()
{
    TemporaryDirectory directory;
    const auto path = directory.path / "1.png";
    write_bytes(path, album_fixture::png(600, 400, 0));
    std::atomic<bool> stop{false};

    REQUIRE(album::display_rotation_image(directory.path, 1, stop)
        == Result::displayed);

    const auto& fake = fake_display::controls();
    // The real guard and its default state directory protect the panel.
    REQUIRE(fake.state_directories
        == std::vector<fs::path>{default_state_directory});
    REQUIRE(fake.show_calls == 1);
    REQUIRE(fake.destroyed == 1);
    REQUIRE(fake.shown.size() == 1);
    REQUIRE(fake.shown[0].size() == 120000);
    REQUIRE(fake.shown[0] == expected_frame(path));
}

void stop_before()
{
    TemporaryDirectory directory;
    std::atomic<bool> stop{true};

    // Cancelled before reading the file, so a missing image is irrelevant.
    REQUIRE(album::display_rotation_image(directory.path, 1, stop)
        == Result::cancelled);
    REQUIRE(fake_display::controls().state_directories.empty());
    REQUIRE(fake_display::controls().show_calls == 0);
}

void invalid_image()
{
    TemporaryDirectory directory;
    write_bytes(directory.path / "2.png", Bytes{'n', 'o', 't', ' ', 'p', 'n', 'g'});
    std::atomic<bool> stop{false};

    expect_throw<std::invalid_argument>([&] {
        album::display_rotation_image(directory.path, 0, stop);
    });

    // A deleted image fails; the worker retries and reconciles.
    expect_throw<std::runtime_error>([&] {
        album::display_rotation_image(directory.path, 1, stop);
    });

    expect_throw<std::invalid_argument>([&] {
        album::display_rotation_image(directory.path, 2, stop);
    });

    // Preparation failures never open a display session.
    REQUIRE(fake_display::controls().state_directories.empty());
    REQUIRE(fake_display::controls().show_calls == 0);
}

void cooldown_retry()
{
    TemporaryDirectory directory;
    const auto path = directory.path / "3.png";
    write_bytes(path, album_fixture::png(600, 400, 123));
    const auto expected = expected_frame(path);
    std::atomic<bool> stop{false};

    auto& fake = fake_display::controls();
    fake.steps = {{Outcome::cooldown, 250}, {Outcome::cooldown, 1}};

    // The frame is prepared once: deleting the file during cooldown does not
    // affect the retry.
    fake.on_show = [&path] {
        std::error_code ignored;
        fs::remove(path, ignored);
    };

    const auto begin = Steady::now();
    REQUIRE(album::display_rotation_image(directory.path, 3, stop)
        == Result::displayed);
    const auto elapsed = Steady::now() - begin;

    // Each retry uses a fresh session, rechecking the real guard.
    REQUIRE(fake.state_directories.size() == 3);
    REQUIRE(fake.destroyed == 3);
    REQUIRE(fake.show_calls == 3);
    REQUIRE(fake.shown.size() == 1);
    REQUIRE(fake.shown[0] == expected);
    REQUIRE(elapsed >= 250ms);
    REQUIRE(elapsed < 5s);
}

void cooldown_cancel()
{
    TemporaryDirectory directory;
    write_bytes(directory.path / "1.png", album_fixture::png());
    std::atomic<bool> stop{false};

    // An enormous remaining time must neither overflow nor block shutdown.
    auto& fake = fake_display::controls();
    fake.steps = {{Outcome::cooldown, std::numeric_limits<std::uint64_t>::max()}};

    // Request stop only once the cooldown has begun; image preparation is
    // slow in sanitizer builds.
    std::atomic<bool> cooling{false};
    Steady::time_point cooldown_start;
    fake.on_show = [&] {
        cooldown_start = Steady::now();
        cooling = true;
    };

    Result result;
    {
        DelayedStop delayed(stop, cooling, 200ms);
        result = album::display_rotation_image(directory.path, 1, stop);
    }
    const auto elapsed = Steady::now() - cooldown_start;

    REQUIRE(result == Result::cancelled);
    REQUIRE(cooling);
    REQUIRE(elapsed >= 200ms);
    REQUIRE(elapsed < 2s);
    REQUIRE(fake.show_calls == 1);
    REQUIRE(fake.shown.empty());
    REQUIRE(fake.state_directories.size() == 1);
}

void stop_before_refresh()
{
    TemporaryDirectory directory;
    write_bytes(directory.path / "1.png", album_fixture::png());
    std::atomic<bool> stop{false};

    // Stop arriving while the session opens: refresh never starts.
    auto& fake = fake_display::controls();
    fake.on_construct = [&stop] { stop = true; };

    REQUIRE(album::display_rotation_image(directory.path, 1, stop)
        == Result::cancelled);
    REQUIRE(fake.state_directories.size() == 1);
    REQUIRE(fake.destroyed == 1);
    REQUIRE(fake.show_calls == 0);
}

void stop_during_refresh()
{
    TemporaryDirectory directory;
    write_bytes(directory.path / "1.png", album_fixture::png());
    std::atomic<bool> stop{false};

    // A refresh in progress is never interrupted, and success is reported.
    auto& fake = fake_display::controls();
    fake.on_show = [&stop] { stop = true; };

    REQUIRE(album::display_rotation_image(directory.path, 1, stop)
        == Result::displayed);
    REQUIRE(fake.show_calls == 1);
    REQUIRE(fake.shown.size() == 1);
}

void display_failure()
{
    TemporaryDirectory directory;
    write_bytes(directory.path / "1.png", album_fixture::png());
    std::atomic<bool> stop{false};

    // Hardware failures propagate without an immediate retry.
    auto& fake = fake_display::controls();
    fake.steps = {{Outcome::failure, 0}};

    expect_throw<std::runtime_error>([&] {
        album::display_rotation_image(directory.path, 1, stop);
    });
    REQUIRE(fake.show_calls == 1);
    REQUIRE(fake.destroyed == 1);
    REQUIRE(fake.shown.empty());
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::pair<std::string, void (*)()>> cases{
            {"success", success},
            {"stop_before", stop_before},
            {"invalid_image", invalid_image},
            {"cooldown_retry", cooldown_retry},
            {"cooldown_cancel", cooldown_cancel},
            {"stop_before_refresh", stop_before_refresh},
            {"stop_during_refresh", stop_during_refresh},
            {"display_failure", display_failure}
        };
        if (argc != 2) {
            throw std::runtime_error("Expected one rotation display test case name");
        }
        for (const auto& entry : cases) {
            if (entry.first == argv[1]) {
                entry.second();
                return 0;
            }
        }
        throw std::runtime_error("Unknown rotation display test case");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
