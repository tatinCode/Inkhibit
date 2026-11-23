#include "inkhibit/cooldown_wait.hpp"
#include "inkhibit/display.hpp"
#include "inkhibit/display_cooldown.hpp"
#include "image_fixtures.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

int inkhibit_test_display_main(int argc, char** argv);

namespace {

namespace fs = std::filesystem;
using inkhibit::DisplayCooldown;
using inkhibit::DisplayGuard;
using Milliseconds = std::uint64_t;

#define REQUIRE(condition) do { \
    if (!(condition)) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); \
} while (false)

fs::path state_dir;
Milliseconds now_ms = 1'000'000;
std::vector<Milliseconds> sleeps;
void (*on_sleep)() = nullptr;
void (*after_construction)() = nullptr;
void (*on_update)() = nullptr;
int sessions = 0;
int show_attempts = 0;
int updates = 0;

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory()
    {
        auto pattern = (fs::current_path() / "cooldown-tests-XXXXXX").string();
        const char* result = ::mkdtemp(pattern.data());
        if (!result) throw std::system_error(errno, std::generic_category(), "mkdtemp");
        path = result;
        state_dir = path / "state";
        fs::create_directory(state_dir);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void seed_idle(Milliseconds deadline)
{
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    std::string id;
    REQUIRE(static_cast<bool>(std::getline(boot, id)) && !id.empty());
    std::ofstream state(state_dir / "display.state");
    state << "INKHIBIT_DISPLAY_V1 " << id << " idle " << deadline << '\n';
    state.close();
    REQUIRE(!state.fail());
}

void require_lock_available()
{
    const int fd = ::open((state_dir / "display.lock").c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const int status = ::flock(fd, LOCK_EX | LOCK_NB);
    ::close(fd);
    REQUIRE(status == 0);
}

template<class Function>
void require_cooldown(Function action, Milliseconds expected, const char* message)
{
    try {
        action();
    } catch (const DisplayCooldown& error) {
        REQUIRE(error.remaining_ms() == expected);
        REQUIRE(std::string(error.what()).find(message) != std::string::npos);
        return;
    }
    throw std::runtime_error("Expected DisplayCooldown");
}

class Capture {
public:
    explicit Capture(std::ostream& stream)
        : stream_(stream), previous_(stream.rdbuf(output.rdbuf())) {}
    ~Capture() { stream_.rdbuf(previous_); }

    std::ostringstream output;

private:
    std::ostream& stream_;
    std::streambuf* previous_;
};

struct CliResult {
    int code;
    std::string out;
    std::string err;
};

fs::path input_path;

CliResult run_cli(const TemporaryDirectory& directory)
{
    input_path = directory.path / "artwork.png";
    {
        std::ofstream fixture(input_path, std::ios::binary);
        fixture.write(reinterpret_cast<const char*>(image_fixtures::png_rgb),
            sizeof image_fixtures::png_rgb);
        fixture.close();
        REQUIRE(!fixture.fail());
    }
    Capture out(std::cout);
    Capture err(std::cerr);
    char program[] = "inkhibit-display";
    char option[] = "--dither";
    char method[] = "nearest";
    auto image = input_path.string();
    char* argv[]{program, option, method, image.data(), nullptr};
    const int code = inkhibit_test_display_main(4, argv);
    return {code, out.output.str(), err.output.str()};
}

void require_completed(const CliResult& result, int code = 0)
{
    REQUIRE(result.code == code);
    REQUIRE(result.err.empty());
    const auto end = result.out.find("Update complete.");
    REQUIRE(end != std::string::npos);
    REQUIRE(result.out.find("Update complete.", end + 1) == std::string::npos);
    REQUIRE(updates == 1);
}

void require_no_completion(const CliResult& result)
{
    REQUIRE(result.out.find("Update complete.") == std::string::npos);
    REQUIRE(updates == 0);
}

void test_durations()
{
    TemporaryDirectory directory;
    now_ms = 179'999;
    require_cooldown([] { DisplayGuard guard(state_dir); }, 1, "1 seconds");
    now_ms = 180'000;
    seed_idle(now_ms + 1250);
    require_cooldown([] { DisplayGuard guard(state_dir); }, 1250, "2 seconds");
    now_ms += 1250;
    DisplayGuard guard(state_dir);
    seed_idle(now_ms + 250);
    require_cooldown([&] { guard.begin(); }, 250, "cooldown active");
}

void test_chunks()
{
    volatile std::sig_atomic_t signal = 0;
    REQUIRE(inkhibit::wait_for_cooldown(0, signal));
    REQUIRE(sleeps.empty());
    signal = SIGINT;
    REQUIRE(!inkhibit::wait_for_cooldown(180'000, signal));
    REQUIRE(sleeps.empty());
    signal = 0;
    REQUIRE(inkhibit::wait_for_cooldown(250, signal));
    const std::vector<Milliseconds> expected{100, 100, 50};
    REQUIRE(sleeps == expected);
}

void test_retry()
{
    TemporaryDirectory directory;
    // Exercise a cooldown raised by show(), after construction acquired the lock.
    after_construction = [] {
        if (sessions == 1) seed_idle(now_ms + 1250);
    };
    on_sleep = [] {
        require_lock_available();
        // The retry must reuse the framebuffer instead of reading the input again.
        if (sleeps.size() == 1) REQUIRE(fs::remove(input_path));
        REQUIRE(updates == 0);
    };
    const auto result = run_cli(directory);
    require_completed(result);
    REQUIRE(result.out.find("waiting 2 seconds") != std::string::npos);
    REQUIRE(sessions == 2 && show_attempts == 2);
    REQUIRE(now_ms == 1'001'250);
    require_lock_available();
}

void test_recheck()
{
    TemporaryDirectory directory;
    seed_idle(now_ms + 150);
    on_sleep = [] {
        require_lock_available();
        REQUIRE(updates == 0);
        // Another session takes the lock and establishes a later deadline.
        if (sleeps.size() == 2) {
            DisplayGuard other(state_dir);
            other.begin();
            other.complete();
        }
    };
    const auto result = run_cli(directory);
    require_completed(result);
    REQUIRE(result.out.find("waiting 1 seconds") != std::string::npos);
    REQUIRE(result.out.find("waiting 180 seconds") != std::string::npos);
    REQUIRE(now_ms == 1'180'150);
    REQUIRE(sessions == 1 && show_attempts == 1);
}

void test_cancel()
{
    TemporaryDirectory directory;
    seed_idle(now_ms + 500);
    on_sleep = [] {
        require_lock_available();
        REQUIRE(std::raise(SIGINT) == 0);
    };
    const auto result = run_cli(directory);
    REQUIRE(result.code == 128 + SIGINT);
    REQUIRE(result.err.empty());
    REQUIRE(result.out.find("Press Ctrl+C to cancel") != std::string::npos);
    require_no_completion(result);
    REQUIRE(sessions == 0 && show_attempts == 0);
    REQUIRE(sleeps == std::vector<Milliseconds>{100});
    require_cooldown([] { DisplayGuard guard(state_dir); }, 400, "cooldown active");
}

void test_recovery()
{
    TemporaryDirectory directory;
    { DisplayGuard abandoned(state_dir); abandoned.begin(); }
    require_cooldown([] { DisplayGuard guard(state_dir); }, 180'000, "recovery cooldown");
    now_ms += 45'000;
    require_cooldown([] { DisplayGuard guard(state_dir); }, 135'000, "cooldown active");
    // Seed another unfinished session, then exercise recovery through the CLI.
    now_ms += 135'000;
    { DisplayGuard abandoned(state_dir); abandoned.begin(); }
    const auto result = run_cli(directory);
    require_completed(result);
    REQUIRE(result.out.find("waiting 180 seconds") != std::string::npos);
    REQUIRE(sessions == 1 && show_attempts == 1);
    REQUIRE(now_ms == 1'360'000);
}

void test_busy()
{
    TemporaryDirectory directory;
    DisplayGuard owner(state_dir);
    const auto result = run_cli(directory);
    REQUIRE(result.code == 1);
    REQUIRE(result.err.find("Another Inkhibit process") != std::string::npos);
    REQUIRE(sleeps.empty() && show_attempts == 0);
    require_no_completion(result);
}

void test_hardware_error()
{
    TemporaryDirectory directory;
    on_update = [] { throw std::runtime_error("Simulated panel failure"); };
    const auto result = run_cli(directory);
    REQUIRE(result.code == 1);
    REQUIRE(result.err.find("Simulated panel failure") != std::string::npos);
    REQUIRE(sleeps.empty() && sessions == 1 && show_attempts == 1);
    require_no_completion(result);
    require_lock_available();
}

void test_signal_during_update()
{
    TemporaryDirectory directory;
    on_update = [] { REQUIRE(std::raise(SIGTERM) == 0); };
    require_completed(run_cli(directory), 128 + SIGTERM);
    REQUIRE(sleeps.empty() && sessions == 1 && show_attempts == 1);
}

} // namespace

extern "C" int inkhibit_guard_test_clock_gettime(clockid_t clock, timespec* now) noexcept
{
    if (clock != CLOCK_BOOTTIME) {
        errno = EINVAL;
        return -1;
    }
    now->tv_sec = static_cast<time_t>(now_ms / 1000);
    now->tv_nsec = static_cast<long>((now_ms % 1000) * 1'000'000);
    return 0;
}

extern "C" int inkhibit_cooldown_test_nanosleep(const timespec* request, timespec*)
{
    REQUIRE(request->tv_sec == 0 && request->tv_nsec > 0);
    REQUIRE(request->tv_nsec % 1'000'000 == 0);
    const auto duration = static_cast<Milliseconds>(request->tv_nsec / 1'000'000);
    REQUIRE(duration <= 100);
    REQUIRE(sleeps.size() < 2000); // Bound a broken retry loop.
    sleeps.push_back(duration);
    now_ms += duration;
    if (on_sleep) on_sleep();
    return 0;
}

namespace inkhibit {

// Real guard and lock, simulated panel. Redirect the CLI's default directory.
Display::Display(const fs::path&) : guard_(state_dir)
{
    ++sessions;
    if (after_construction) after_construction();
}

Display::~Display() noexcept = default;

void Display::show(const framebuffer::Buffer& buffer)
{
    REQUIRE(buffer.size() == framebuffer_size);
    REQUIRE(!attempted_);
    attempted_ = true;
    ++show_attempts;
    guard_.begin();
    if (on_update) on_update();
    guard_.complete();
    ++updates;
}

} // namespace inkhibit

int main(int argc, char** argv)
{
    try {
        REQUIRE(argc == 2);
        const std::string name = argv[1];
        const struct { const char* name; void (*run)(); } cases[]{
            {"durations", test_durations}, {"chunks", test_chunks},
            {"retry", test_retry}, {"recheck", test_recheck},
            {"cancel", test_cancel}, {"recovery", test_recovery},
            {"busy", test_busy}, {"hardware_error", test_hardware_error},
            {"signal_during_update", test_signal_during_update}
        };
        for (const auto& test : cases) {
            if (name == test.name) {
                test.run();
                return 0;
            }
        }
        throw std::runtime_error("Unknown test: " + name);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
