#include "inkhibit/album/rotation_store.hpp"
#include "inkhibit/album/rotation_worker.hpp"
#include "inkhibit/album/store.hpp"
#include "album_png_fixture.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace album = inkhibit::album;
namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
using Steady = std::chrono::steady_clock;
using Ids = std::vector<album::ImageId>;
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

// Generous bound for events that should happen promptly, even on the Pi.
constexpr auto prompt = 5s;

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory()
    {
        auto pattern =
            (fs::current_path() / "rotation-worker-tests-XXXXXX").string();
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

// Records display attempts and reported errors from the worker thread.
struct Recorder {
    std::mutex mutex;
    std::condition_variable changed;
    Ids displays;
    std::vector<Steady::time_point> display_times;
    std::vector<std::string> errors;
    std::vector<Steady::time_point> error_times;

    // Decides each display outcome; may throw. Called without the mutex held.
    std::function<album::RotationDisplayResult(
        album::ImageId, const std::atomic<bool>&)> behavior;

    album::RotationDisplay display()
    {
        return [this](album::ImageId id, const std::atomic<bool>& stop) {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                displays.push_back(id);
                display_times.push_back(Steady::now());
            }
            changed.notify_all();
            return behavior
                ? behavior(id, stop)
                : album::RotationDisplayResult::displayed;
        };
    }

    album::RotationErrorHandler errors_handler()
    {
        return [this](const std::string& message) {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                errors.push_back(message);
                error_times.push_back(Steady::now());
            }
            changed.notify_all();
        };
    }

    template<class Predicate>
    bool wait(Predicate predicate, Steady::duration timeout = prompt)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return predicate(*this); });
    }

    Ids snapshot_displays()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return displays;
    }

    std::vector<std::string> snapshot_errors()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return errors;
    }
};

// Runs the worker on its own thread. Always stops and joins, even when a
// REQUIRE fails, so the worker never outlives the store or recorder.
class Running {
public:
    explicit Running(album::RotationWorker& worker)
        : worker_(worker), thread_([this] { worker_.run(); })
    {
    }

    ~Running()
    {
        if (thread_.joinable()) {
            worker_.request_stop();
            thread_.join();
        }
    }

    // Returns how long shutdown took.
    Steady::duration stop()
    {
        const auto begin = Steady::now();
        worker_.request_stop();
        thread_.join();
        return Steady::now() - begin;
    }

    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;

private:
    album::RotationWorker& worker_;
    std::thread thread_;
};

bool wait_for_file(const fs::path& path)
{
    const auto deadline = Steady::now() + prompt;
    while (Steady::now() < deadline) {
        if (fs::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return fs::exists(path);
}

fs::path state_file(const fs::path& root)
{
    return root / ".rotation-state";
}

void add_images(album::AlbumStore& store, int count)
{
    for (int i = 0; i < count; ++i) {
        store.add(album_fixture::png());
    }
}

void constructor_validation()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    Recorder recorder;

    expect_throw<std::invalid_argument>([&] {
        album::RotationWorker worker(
            store, directory.path, nullptr, recorder.errors_handler());
    });
    expect_throw<std::invalid_argument>([&] {
        album::RotationWorker worker(
            store, directory.path, recorder.display(), nullptr);
    });
}

void stop_before_run()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    add_images(store, 2);
    Recorder recorder;
    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());

    // run() returns without loading, saving, or displaying.
    worker.request_stop();
    worker.run();
    REQUIRE(recorder.snapshot_displays().empty());
    REQUIRE(recorder.snapshot_errors().empty());
    REQUIRE(!fs::exists(state_file(directory.path)));
}

void first_display()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    add_images(store, 2);
    Recorder recorder;
    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());

    const auto before = Clock::now();
    Running running(worker);
    REQUIRE(recorder.wait([](Recorder& r) { return !r.displays.empty(); }));

    // After the first display, the worker waits for the hour-long interval.
    std::this_thread::sleep_for(200ms);
    REQUIRE(running.stop() < prompt);
    const auto after = Clock::now();

    REQUIRE(recorder.snapshot_displays() == Ids{1});
    REQUIRE(recorder.snapshot_errors().empty());

    const auto saved = album::load_rotation_state(directory.path);
    REQUIRE(saved);
    REQUIRE(saved->order == (Ids{1, 2}));
    REQUIRE(saved->remaining == Ids{2});
    REQUIRE(saved->history == Ids{1});
    REQUIRE(saved->history_position == 0u);
    REQUIRE(saved->next_change);

    // The file stores whole seconds; allow for truncation.
    REQUIRE(*saved->next_change >= before + album::default_rotation_interval - 1s);
    REQUIRE(*saved->next_change <= after + album::default_rotation_interval);
}

void resume_waiting()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    add_images(store, 2);

    album::RotationState saved;
    saved.order = {1, 2};
    saved.remaining = {2};
    saved.history = {1};
    saved.history_position = 0;
    const auto deadline = std::chrono::time_point_cast<std::chrono::seconds>(
        Clock::now() + 1h);
    saved.next_change = deadline;
    album::save_rotation_state(directory.path, saved);

    Recorder recorder;
    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());
    Running running(worker);

    // A restart keeps the saved deadline instead of displaying immediately.
    std::this_thread::sleep_for(300ms);
    REQUIRE(running.stop() < prompt);
    REQUIRE(recorder.snapshot_displays().empty());
    REQUIRE(recorder.snapshot_errors().empty());

    const auto reloaded = album::load_rotation_state(directory.path);
    REQUIRE(reloaded);
    REQUIRE(reloaded->history == Ids{1});
    REQUIRE(reloaded->next_change == deadline);
}

void album_change_wakes()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    Recorder recorder;
    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());
    Running running(worker);

    // An empty album is saved on first start, then the worker waits
    // indefinitely. The save happens after listing, so the upload below
    // cannot be missed.
    REQUIRE(wait_for_file(state_file(directory.path)));
    std::this_thread::sleep_for(100ms);
    REQUIRE(recorder.snapshot_displays().empty());

    add_images(store, 1);
    worker.notify_album_changed();
    REQUIRE(recorder.wait([](Recorder& r) { return !r.displays.empty(); }));

    // The single image is shown once and never refreshed again.
    std::this_thread::sleep_for(300ms);
    REQUIRE(running.stop() < prompt);
    REQUIRE(recorder.snapshot_displays() == Ids{1});
    REQUIRE(recorder.snapshot_errors().empty());
}

void stop_cancels_display()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    add_images(store, 2);
    Recorder recorder;

    // Simulate waiting for the hardware cooldown, which honors shutdown.
    recorder.behavior = [](album::ImageId, const std::atomic<bool>& stop) {
        const auto deadline = Steady::now() + 30s;
        while (!stop.load() && Steady::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        return album::RotationDisplayResult::cancelled;
    };

    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());
    Running running(worker);
    REQUIRE(recorder.wait([](Recorder& r) { return !r.displays.empty(); }));
    REQUIRE(running.stop() < prompt);
    REQUIRE(recorder.snapshot_errors().empty());

    // Nothing was recorded as shown; the target stays queued.
    const auto saved = album::load_rotation_state(directory.path);
    REQUIRE(saved);
    REQUIRE(saved->history.empty());
    REQUIRE(saved->remaining == (Ids{1, 2}));
}

void error_backoff()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    add_images(store, 2);
    Recorder recorder;
    std::atomic<int> attempts{0};
    recorder.behavior = [&attempts](album::ImageId, const std::atomic<bool>&) {
        if (attempts.fetch_add(1) == 0) {
            throw std::runtime_error("panel failed");
        }
        return album::RotationDisplayResult::displayed;
    };

    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());
    Running running(worker);
    REQUIRE(recorder.wait([](Recorder& r) { return !r.errors.empty(); }));

    // Album notifications must not bypass the retry backoff.
    worker.notify_album_changed();

    REQUIRE(recorder.wait(
        [](Recorder& r) { return r.displays.size() == 2; }, 15s));
    REQUIRE(running.stop() < prompt);

    Steady::duration gap;
    {
        const std::lock_guard<std::mutex> lock(recorder.mutex);
        REQUIRE(recorder.errors == std::vector<std::string>{"panel failed"});
        REQUIRE(recorder.displays == (Ids{1, 1})); // Same target retried.
        gap = recorder.display_times[1] - recorder.error_times[0];
    }
    REQUIRE(gap >= 4500ms);

    const auto saved = album::load_rotation_state(directory.path);
    REQUIRE(saved);
    REQUIRE(saved->history == Ids{1});
    REQUIRE(saved->remaining == Ids{2});
}

void stop_during_backoff()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    add_images(store, 2);
    Recorder recorder;
    recorder.behavior = [](album::ImageId, const std::atomic<bool>&)
            -> album::RotationDisplayResult {
        throw 42; // Not derived from std::exception.
    };

    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());
    Running running(worker);
    REQUIRE(recorder.wait([](Recorder& r) { return !r.errors.empty(); }));

    // Shutdown interrupts the five-second backoff.
    REQUIRE(running.stop() < 2s);
    REQUIRE(recorder.snapshot_errors()
        == std::vector<std::string>{"Unknown rotation worker error"});
    REQUIRE(recorder.snapshot_displays() == Ids{1});

    const auto saved = album::load_rotation_state(directory.path);
    REQUIRE(saved);
    REQUIRE(saved->history.empty());
}

void corrupt_state()
{
    TemporaryDirectory directory;
    album::AlbumStore store(directory.path);
    add_images(store, 2);
    {
        std::ofstream out(state_file(directory.path), std::ios::binary);
        out << "not a rotation state\n";
        REQUIRE(out);
    }

    Recorder recorder;
    album::RotationWorker worker(
        store, directory.path, recorder.display(), recorder.errors_handler());
    Running running(worker);

    // Malformed state is reported and never overwritten or displayed past.
    REQUIRE(recorder.wait([](Recorder& r) { return !r.errors.empty(); }));
    REQUIRE(running.stop() < 2s);
    REQUIRE(recorder.snapshot_displays().empty());

    std::ifstream in(state_file(directory.path), std::ios::binary);
    std::string line;
    REQUIRE(std::getline(in, line));
    REQUIRE(line == "not a rotation state");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::pair<std::string, void (*)()>> cases{
            {"constructor_validation", constructor_validation},
            {"stop_before_run", stop_before_run},
            {"first_display", first_display},
            {"resume_waiting", resume_waiting},
            {"album_change_wakes", album_change_wakes},
            {"stop_cancels_display", stop_cancels_display},
            {"error_backoff", error_backoff},
            {"stop_during_backoff", stop_during_backoff},
            {"corrupt_state", corrupt_state}
        };
        if (argc != 2) {
            throw std::runtime_error("Expected one rotation worker test case name");
        }
        for (const auto& entry : cases) {
            if (entry.first == argv[1]) {
                entry.second();
                return 0;
            }
        }
        throw std::runtime_error("Unknown rotation worker test case");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
