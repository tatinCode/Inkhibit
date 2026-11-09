#include "inkhibit/display.hpp"
#include "inkhibit/display_guard.hpp"
#include "inkhibit/driver_status.h"
#include "fake_hardware.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using inkhibit::Display;
using inkhibit::DisplayGuard;
namespace fs = std::filesystem;

// Driver delays advance fake.now_ns; the guard observes the same elapsed time,
// offset to a mature Linux boot. No real sleeps or changes to the host clock.
std::uint64_t boot_base_ms = 1'000'000;
bool fail_boot_clock = false;

std::uint64_t now_ms()
{
    return boot_base_ms + fake.now_ns / NS_PER_MS;
}

#define REQUIRE(condition)                                                \
    do {                                                                  \
        if (!(condition)) {                                               \
            throw std::runtime_error(std::string(__FILE__) + ":"           \
                + std::to_string(__LINE__) + ": " + #condition);           \
        }                                                                 \
    } while (false)

template<class Exception = std::runtime_error, class Function>
void expect_throw(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const Exception& error) {
        if (std::string(error.what()).find(message) == std::string::npos) {
            throw std::runtime_error("Expected exception containing '" + message
                + "', got: " + error.what());
        }
        return;
    }
    throw std::runtime_error("Expected exception containing: " + message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        // CTest's working directory is the build tree, never /var/lib/inkhibit.
        std::string pattern = (fs::current_path() / "display-tests-XXXXXX").string();
        char* result = ::mkdtemp(pattern.data());
        if (!result) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        path = result;
    }

    ~TemporaryDirectory() noexcept
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    fs::path path;
};

std::string read_file(const fs::path& path)
{
    std::ifstream input(path);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::string& contents)
{
    std::ofstream output(path);
    output << contents;
    output.close();
    REQUIRE(!output.fail());
}

std::string boot_id()
{
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string id;
    REQUIRE(static_cast<bool>(std::getline(input, id)));
    REQUIRE(!id.empty());
    return id;
}

struct State {
    std::string phase;
    std::uint64_t deadline;
};

State read_state(const fs::path& directory)
{
    std::istringstream input(read_file(directory / "display.state"));
    std::string version, boot, extra;
    State state{};
    REQUIRE(static_cast<bool>(input >> version >> boot >> state.phase >> state.deadline));
    REQUIRE(!(input >> extra));
    REQUIRE(version == "INKHIBIT_DISPLAY_V1");
    REQUIRE(boot == boot_id());
    return state;
}

void seed_state(const fs::path& directory, const std::string& phase,
                const std::string& deadline, const std::string& boot = boot_id())
{
    write_file(directory / "display.state",
        "INKHIBIT_DISPLAY_V1 " + boot + " " + phase + " " + deadline + "\n");
}

void require_state(const fs::path& directory, const std::string& phase,
                   std::uint64_t deadline)
{
    const auto state = read_state(directory);
    REQUIRE(state.phase == phase);
    REQUIRE(state.deadline == deadline);
    REQUIRE(!fs::exists(directory / "display.state.tmp"));
}

void require_no_hardware()
{
    REQUIRE(fake.chip_opens == 0);
    REQUIRE(fake.spi_opens == 0);
    REQUIRE(fake.writes == 0);
    REQUIRE(fake.reads == 0);
    REQUIRE(fake.spi_calls == 0);
}

void require_closed()
{
    REQUIRE(!fake.chip_open);
    REQUIRE(!fake.spi_open);
    for (const int claimed : fake.claimed) {
        REQUIRE(claimed == 0);
    }
}

std::vector<std::uint8_t> valid_frame()
{
    constexpr std::array<std::uint8_t, 6> colors{0, 1, 2, 3, 5, 6};
    std::vector<std::uint8_t> frame(Display::framebuffer_size);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        frame[i] = static_cast<std::uint8_t>(
            (colors[i % colors.size()] << 4) | colors[(i + 1) % colors.size()]);
    }
    return frame;
}

// exec closes inherited O_CLOEXEC guard descriptors, so these really are
// independent sessions. No timing-sensitive sleeps or pipe handshakes needed.
int child_session(const char* action, const fs::path& directory)
{
    const std::string path = directory.string();
    const std::string time = std::to_string(now_ms());
    const char* args[]{"inkhibit_display_tests", "--child", action,
                       path.c_str(), time.c_str(), nullptr};
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::execv("/proc/self/exe", const_cast<char* const*>(args));
        ::_exit(127);
    }
    int status = 0;
    pid_t result;
    do {
        result = ::waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    REQUIRE(result == pid);
    REQUIRE(WIFEXITED(status));
    return WEXITSTATUS(status);
}

int run_child(const std::string& action, const fs::path& directory)
{
    if (action == "lock_probe") {
        const int fd = ::open((directory / "display.lock").c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        const int result = ::flock(fd, LOCK_EX | LOCK_NB);
        const int error = errno;
        REQUIRE(::close(fd) == 0);
        if (result == 0) {
            return 0;
        }
        REQUIRE(error == EWOULDBLOCK || error == EAGAIN);
        return 2;
    }
    try {
        DisplayGuard guard(directory);
        if (action == "complete") {
            guard.begin();
            guard.complete();
        } else if (action == "abandon") {
            guard.begin();
            ::_exit(0); // Simulate process death: skip all C++ destructors.
        } else {
            REQUIRE(action == "probe");
        }
        return 0;
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        if (action == "probe" && message.find("Another Inkhibit process") != std::string::npos) {
            return 2;
        }
        if (action == "probe" && message.find("cooldown active") != std::string::npos) {
            return 3;
        }
        throw;
    }
}

void test_lazy_init()
{
    TemporaryDirectory directory;
    {
        Display display(directory.path);
        display.close();
        display.close();
        require_no_hardware();
        REQUIRE(!fs::exists(directory.path / "display.state"));
    }
    REQUIRE(child_session("probe", directory.path) == 0);
}

void test_frame_size(std::size_t size)
{
    TemporaryDirectory directory;
    Display display(directory.path);
    expect_throw<std::invalid_argument>([&] {
        display.show(std::vector<std::uint8_t>(size, 0x11));
    }, "exactly 120000 bytes");
    require_no_hardware();
    REQUIRE(!fs::exists(directory.path / "display.state"));
}

void test_invalid_colors()
{
    TemporaryDirectory directory;
    Display display(directory.path);
    auto frame = valid_frame();
    // Every unsupported nibble, in either pixel position, at both ends of the
    // buffer. This catches partial scans as well as checking only one nibble.
    for (unsigned color = 0; color < 16; ++color) {
        if (color <= 3 || color == 5 || color == 6) {
            continue;
        }
        for (const auto position : {std::size_t{0}, frame.size() - 1}) {
            const auto saved = frame[position];
            for (const auto byte : {color, (color << 4) | 1u}) {
                frame[position] = static_cast<std::uint8_t>(byte);
                expect_throw<std::invalid_argument>([&] { display.show(frame); }, "color code");
            }
            frame[position] = saved;
        }
    }
    require_no_hardware();
    REQUIRE(!fs::exists(directory.path / "display.state"));
}

void test_validation_retry()
{
    TemporaryDirectory directory;
    Display display(directory.path);
    expect_throw<std::invalid_argument>([&] { display.show({}); }, "120000");
    display.show(valid_frame());
    REQUIRE(fake.chip_opens == 1);
    require_closed();
}

void test_success()
{
    TemporaryDirectory directory;
    const auto frame = valid_frame();
    const auto original = frame;
    {
        Display display(directory.path);
        display.show(frame);
        REQUIRE(frame == original);
        REQUIRE(inkhibit_driver_error() == 0);
        REQUIRE(fake.chip_opens == 1 && fake.spi_opens == 1);
        REQUIRE(fake.chip_closes == 1 && fake.spi_closes == 1);
        REQUIRE(fake.frees == 4);
        REQUIRE(fake.claimed[8] == 0);
        REQUIRE(fake.levels[8] == 1);
        REQUIRE(fake.levels[18] == 0);
        require_closed();
        require_state(directory.path, "idle", now_ms() + DisplayGuard::cooldown_ms);

        std::vector<std::uint8_t> commands;
        std::size_t data_start = fake.trace_size;
        for (std::size_t i = 0; i < fake.trace_size; ++i) {
            REQUIRE(fake.trace[i].cs == 0);
            if (fake.trace[i].dc == 0) {
                commands.push_back(fake.trace[i].value);
                if (fake.trace[i].value == 0x10) {
                    data_start = i + 1;
                }
            }
        }
        const std::array<std::uint8_t, 6> tail{0x10, 0x04, 0x06, 0x12, 0x02, 0x07};
        REQUIRE(commands.size() >= tail.size());
        REQUIRE(std::equal(tail.begin(), tail.end(), commands.end() - tail.size()));
        REQUIRE(data_start + frame.size() < fake.trace_size);
        for (std::size_t i = 0; i < frame.size(); ++i) {
            REQUIRE(fake.trace[data_start + i].dc == 1);
            REQUIRE(fake.trace[data_start + i].value == frame[i]);
        }
        REQUIRE(fake.trace[fake.trace_size - 1].dc == 1);
        REQUIRE(fake.trace[fake.trace_size - 1].value == 0xA5);

        const auto calls = fake.spi_calls;
        const auto state = read_file(directory.path / "display.state");
        expect_throw<std::logic_error>([&] { display.show(frame); }, "one update");
        display.close();
        display.close();
        REQUIRE(fake.spi_calls == calls);
        REQUIRE(read_file(directory.path / "display.state") == state);
        REQUIRE(child_session("lock_probe", directory.path) == 2);
    }
    REQUIRE(fake.chip_closes == 1 && fake.spi_closes == 1);
    REQUIRE(child_session("lock_probe", directory.path) == 0);
}

void test_init_failure(unsigned stage)
{
    REQUIRE(stage >= 1 && stage <= 7);
    TemporaryDirectory directory;
    fake.fail_acquisition_at = stage;
    {
        Display display(directory.path);
        expect_throw([&] { display.show(valid_frame()); }, "Initializing GPIO/SPI");
        REQUIRE(fake.acquisition_step == stage);
        REQUIRE(inkhibit_driver_error() == EIO);
        require_closed();
        require_state(directory.path, "active", 0);
    }
    expect_throw([&] { Display display(directory.path); }, "recovery cooldown");
    require_state(directory.path, "idle", now_ms() + DisplayGuard::cooldown_ms);
}

void test_one_shot_after_failure()
{
    TemporaryDirectory directory;
    fake.fail_acquisition_at = 1;
    Display display(directory.path);
    expect_throw([&] { display.show(valid_frame()); }, "Initializing GPIO/SPI");
    fake.fail_acquisition_at = 0;
    expect_throw<std::logic_error>([&] { display.show(valid_frame()); }, "one update");
    REQUIRE(fake.chip_opens == 1);
    require_state(directory.path, "active", 0);
}

void test_hardware_failure(const std::string& operation, int error)
{
    TemporaryDirectory directory;
    {
        Display display(directory.path);
        expect_throw([&] { display.show(valid_frame()); }, operation);
        REQUIRE(inkhibit_driver_error() == error);
        REQUIRE(fake.chip_closes == 1 && fake.spi_closes == 1);
        REQUIRE(fake.frees == 4);
        REQUIRE(fake.claimed[8] == 0);
        REQUIRE(fake.levels[8] == 1);
        require_closed();
        require_state(directory.path, "active", 0);
        display.close(); // Already-cleaned failure must not release resources twice.
    }
    REQUIRE(fake.chip_closes == 1 && fake.spi_closes == 1);
}

void test_state_write_error()
{
    TemporaryDirectory directory;
    Display display(directory.path);
    fs::create_directory(directory.path / "display.state.tmp");
    expect_throw<std::system_error>([&] { display.show(valid_frame()); }, "temporary display state");
    require_no_hardware();
    REQUIRE(!fs::exists(directory.path / "display.state"));
}

void test_cooldown_before_hardware()
{
    TemporaryDirectory directory;
    seed_state(directory.path, "idle", std::to_string(now_ms() + 1));
    expect_throw([&] { Display display(directory.path); }, "cooldown active");
    require_no_hardware();
}

void test_lock_before_hardware()
{
    TemporaryDirectory directory;
    Display first(directory.path);
    expect_throw([&] { Display second(directory.path); }, "Another Inkhibit process");
    require_no_hardware();
}

void test_initial_uptime()
{
    TemporaryDirectory directory;
    boot_base_ms = DisplayGuard::cooldown_ms - 1;
    expect_throw([&] { DisplayGuard guard(directory.path); }, "1 seconds");
    REQUIRE(!fs::exists(directory.path / "display.state"));
    boot_base_ms = DisplayGuard::cooldown_ms;
    DisplayGuard guard(directory.path);
    guard.begin();
    require_state(directory.path, "active", 0);
}

void test_lifecycle()
{
    TemporaryDirectory directory;
    {
        DisplayGuard guard(directory.path);
        REQUIRE(!fs::exists(directory.path / "display.state"));
        guard.begin();
        require_state(directory.path, "active", 0);
        boot_base_ms += 20'000;
        guard.complete();
        require_state(directory.path, "idle", now_ms() + DisplayGuard::cooldown_ms);
    }
    // The lock filename/inode is independent of atomically replaced state.
    REQUIRE(child_session("lock_probe", directory.path) == 0);
}

void test_begin_twice()
{
    TemporaryDirectory directory;
    DisplayGuard guard(directory.path);
    guard.begin();
    expect_throw<std::logic_error>([&] { guard.begin(); }, "already started");
    require_state(directory.path, "active", 0);
}

void test_complete_idempotent()
{
    TemporaryDirectory directory;
    DisplayGuard guard(directory.path);
    guard.complete();
    REQUIRE(!fs::exists(directory.path / "display.state"));
    guard.begin();
    guard.complete();
    const auto original = read_file(directory.path / "display.state");
    boot_base_ms += 1000;
    guard.complete();
    REQUIRE(read_file(directory.path / "display.state") == original);
}

void test_rechecks_state()
{
    TemporaryDirectory directory;
    DisplayGuard guard(directory.path);
    seed_state(directory.path, "idle", std::to_string(now_ms() + 1000));
    expect_throw([&] { guard.begin(); }, "cooldown active");
    require_state(directory.path, "idle", now_ms() + 1000);
}

void test_lock_process()
{
    TemporaryDirectory directory;
    {
        DisplayGuard guard(directory.path);
        REQUIRE(child_session("probe", directory.path) == 2);
        guard.begin();
        guard.complete();
        REQUIRE(child_session("lock_probe", directory.path) == 2);
    }
    REQUIRE(child_session("lock_probe", directory.path) == 0);
}

void test_cooldown_process()
{
    TemporaryDirectory directory;
    REQUIRE(child_session("complete", directory.path) == 0);
    const auto deadline = now_ms() + DisplayGuard::cooldown_ms;
    require_state(directory.path, "idle", deadline);
    REQUIRE(child_session("probe", directory.path) == 3);
    // A throwing constructor must also release the process lock.
    REQUIRE(child_session("lock_probe", directory.path) == 0);
    boot_base_ms = deadline - 1;
    REQUIRE(child_session("probe", directory.path) == 3);
    boot_base_ms = deadline;
    REQUIRE(child_session("probe", directory.path) == 0);
    require_no_hardware();
}

void verify_recovery(const fs::path& directory)
{
    expect_throw([&] { DisplayGuard guard(directory); }, "recovery cooldown");
    const auto deadline = now_ms() + DisplayGuard::cooldown_ms;
    require_state(directory, "idle", deadline);
    const auto recorded = read_file(directory / "display.state");
    boot_base_ms += 45'000;
    expect_throw([&] { DisplayGuard guard(directory); }, "cooldown active");
    REQUIRE(read_file(directory / "display.state") == recorded);
    boot_base_ms = deadline;
    REQUIRE(child_session("probe", directory) == 0);
    require_no_hardware();
}

void test_interrupted_process()
{
    TemporaryDirectory directory;
    REQUIRE(child_session("abandon", directory.path) == 0);
    require_state(directory.path, "active", 0);
    verify_recovery(directory.path);
}

void test_invalid_state()
{
    const std::string prefix = "INKHIBIT_DISPLAY_V1 " + boot_id() + " ";
    const std::array<std::string, 8> malformed{
        "", "garbage", "INKHIBIT_DISPLAY_V0 " + boot_id() + " idle 0",
        prefix + "idle", prefix + "sleeping 0", prefix + "idle nan",
        prefix + "idle 18446744073709551616", prefix + "idle 0 trailing"
    };
    for (const auto& contents : malformed) {
        TemporaryDirectory directory;
        write_file(directory.path / "display.state", contents);
        verify_recovery(directory.path);
    }
}

void test_negative_timestamp()
{
    TemporaryDirectory directory;
    seed_state(directory.path, "idle", "-1");
    // A negative boot timestamp is corrupt state, not a centuries-long cooldown.
    verify_recovery(directory.path);
}

void test_boot_change()
{
    TemporaryDirectory directory;
    seed_state(directory.path, "idle", "0", "different-boot-id");
    verify_recovery(directory.path);
}

void test_clock_failure()
{
    TemporaryDirectory directory;
    fail_boot_clock = true;
    expect_throw<std::system_error>([&] { DisplayGuard guard(directory.path); }, "Reading boot clock");
    fail_boot_clock = false;
    REQUIRE(child_session("probe", directory.path) == 0);
    require_no_hardware();
}

void test_nonregular_state()
{
    TemporaryDirectory directory;
    fs::create_directory(directory.path / "display.state");
    expect_throw([&] { DisplayGuard guard(directory.path); }, "regular file");
    fs::remove(directory.path / "display.state");
    REQUIRE(::mkfifo((directory.path / "display.state").c_str(), 0600) == 0);
    expect_throw([&] { DisplayGuard guard(directory.path); }, "regular file");
    require_no_hardware();
}

void test_symlinks()
{
    for (const char* name : {"display.lock", "display.state", "display.state.tmp"}) {
        TemporaryDirectory directory;
        const auto target = directory.path / "sentinel";
        write_file(target, "untouched\n");
        fs::create_symlink(target, directory.path / name);
        expect_throw<std::system_error>([&] {
            DisplayGuard guard(directory.path);
            guard.begin();
        }, "Opening");
        REQUIRE(read_file(target) == "untouched\n");
    }
    require_no_hardware();
}

void test_oversized_state()
{
    TemporaryDirectory directory;
    const std::string oversized(257, 'x');
    write_file(directory.path / "display.state", oversized);
    expect_throw([&] { DisplayGuard guard(directory.path); }, "unexpectedly large");
    REQUIRE(read_file(directory.path / "display.state") == oversized);
    require_no_hardware();
}

template<class T>
constexpr bool exclusive_owner = !std::is_copy_constructible_v<T>
    && !std::is_copy_assignable_v<T> && !std::is_move_constructible_v<T>
    && !std::is_move_assignable_v<T> && std::is_nothrow_destructible_v<T>;

static_assert(exclusive_owner<Display>);
static_assert(exclusive_owner<DisplayGuard>);

} // namespace

// The test copy of <time.h> declares this C symbol when clock_gettime is
// remapped. Match glibc's noexcept declaration; keep normal headers elsewhere.
extern "C" int inkhibit_guard_test_clock_gettime(clockid_t clock, timespec* now) noexcept
{
    if (fail_boot_clock || clock != CLOCK_BOOTTIME || now == nullptr) {
        errno = EIO;
        return -1;
    }
    const auto milliseconds = now_ms();
    now->tv_sec = static_cast<time_t>(milliseconds / 1000);
    now->tv_nsec = static_cast<long>((milliseconds % 1000) * 1'000'000);
    return 0;
}

int main(int argc, char** argv)
{
    try {
        if (argc == 5 && std::string(argv[1]) == "--child") {
            boot_base_ms = std::stoull(argv[4]);
            return run_child(argv[2], argv[3]);
        }
        REQUIRE(argc >= 2);
        const std::string name = argv[1];
        if (name == "display.init_failure") {
            REQUIRE(argc == 3);
            test_init_failure(static_cast<unsigned>(std::stoul(argv[2])));
        } else {
            REQUIRE(argc == 2);
            const std::pair<const char*, void (*)()> tests[]{
                {"display.lazy_init", test_lazy_init},
                {"display.empty_frame", [] { test_frame_size(0); }},
                {"display.short_frame", [] { test_frame_size(Display::framebuffer_size - 1); }},
                {"display.long_frame", [] { test_frame_size(Display::framebuffer_size + 1); }},
                {"display.invalid_colors", test_invalid_colors},
                {"display.validation_retry", test_validation_retry},
                {"display.success", test_success},
                {"display.one_shot_after_failure", test_one_shot_after_failure},
                {"display.panel_init_timeout", [] {
                    fake.ready_at_ns = std::numeric_limits<std::uint64_t>::max();
                    test_hardware_failure("Initializing panel", ETIMEDOUT);
                }},
                {"display.transmission_error", [] {
                    fake.fail_spi_command_enabled = 1;
                    fake.fail_spi_command = 0x10;
                    test_hardware_failure("Displaying framebuffer", EIO);
                }},
                {"display.refresh_timeout", [] {
                    // Initialization has two successful BUSY waits. Fail the
                    // subsequent DISPLAY_REFRESH wait (after POWER_ON).
                    fake.busy_from_read = 4;
                    test_hardware_failure("Displaying framebuffer", ETIMEDOUT);
                }},
                {"display.sleep_error", [] {
                    fake.fail_spi_command_enabled = 1;
                    fake.fail_spi_command = 0x07;
                    test_hardware_failure("Shutting down display", EIO);
                }},
                {"display.cleanup_error", [] {
                    fake.fail_free_pin = 17;
                    test_hardware_failure("Shutting down display", EIO);
                }},
                {"display.state_write_error", test_state_write_error},
                {"display.cooldown_before_hardware", test_cooldown_before_hardware},
                {"display.lock_before_hardware", test_lock_before_hardware},
                {"guard.initial_uptime", test_initial_uptime},
                {"guard.lifecycle", test_lifecycle},
                {"guard.begin_twice", test_begin_twice},
                {"guard.complete_idempotent", test_complete_idempotent},
                {"guard.rechecks_state", test_rechecks_state},
                {"guard.lock_process", test_lock_process},
                {"guard.cooldown_process", test_cooldown_process},
                {"guard.interrupted_process", test_interrupted_process},
                {"guard.invalid_state", test_invalid_state},
                {"guard.negative_timestamp", test_negative_timestamp},
                {"guard.boot_change", test_boot_change},
                {"guard.clock_failure", test_clock_failure},
                {"guard.nonregular_state", test_nonregular_state},
                {"guard.symlinks", test_symlinks},
                {"guard.oversized_state", test_oversized_state}
            };
            const auto found = std::find_if(std::begin(tests), std::end(tests),
                [&](const auto& test) { return name == test.first; });
            REQUIRE(found != std::end(tests));
            found->second();
        }
        require_closed();
        std::cout << "PASS: " << argv[1] << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << (argc > 1 ? argv[1] : "missing test name")
                  << ": " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
