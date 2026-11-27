#include "inkhibit/album/rotation_step.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace album = inkhibit::album;
using Clock = std::chrono::system_clock;
using Result = album::RotationStepResult;
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

// Arbitrary fixed wall-clock start, far from either time_point limit.
const Clock::time_point start{std::chrono::seconds{1'800'000'000}};

// Simulated panel refresh duration, so completion differs from selection time.
constexpr auto refresh_duration = 30s;

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

album::RotationState make_state(
    std::vector<album::ImageId> order,
    std::vector<album::ImageId> remaining,
    std::vector<album::ImageId> history,
    std::optional<std::size_t> position,
    std::optional<Clock::time_point> next_change)
{
    album::RotationState state;
    state.order = std::move(order);
    state.remaining = std::move(remaining);
    state.history = std::move(history);
    state.history_position = position;
    state.next_change = next_change;
    return state;
}

// Fake storage, album, clock, and panel. Every callback logs an event so tests
// can verify ordering, e.g. that the queue is saved before display.
struct Harness {
    std::vector<album::ImageId> images;
    std::optional<album::RotationState> stored;
    bool load_throws = false;

    std::vector<album::RotationState> saves; // successful saves only
    int save_failures = 0;
    std::function<void()> on_save;

    Clock::time_point time = start;
    int now_failures = 0;

    std::vector<album::ImageId> displays; // every display attempt
    std::function<album::RotationDisplayResult(album::ImageId)> on_display;

    std::vector<std::string> events;
    std::atomic<bool> stop{false};
    std::mt19937 random{12345};
    album::RotationStepState state;

    Harness() = default;
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    album::RotationStepCallbacks callbacks()
    {
        album::RotationStepCallbacks result;

        result.list_images = [this] {
            events.push_back("list");
            return images;
        };

        result.load_state = [this] {
            events.push_back("load");
            if (load_throws) {
                throw std::runtime_error("load failed");
            }
            return stored;
        };

        result.save_state = [this](const album::RotationState& saved) {
            events.push_back("save");
            if (on_save) {
                on_save();
            }
            if (save_failures > 0) {
                --save_failures;
                throw std::runtime_error("save failed");
            }
            saves.push_back(saved);
            stored = saved;
        };

        result.display = [this](
                album::ImageId id, const std::atomic<bool>& stop_requested) {
            REQUIRE(&stop_requested == &stop);
            events.push_back("display:" + std::to_string(id));
            displays.push_back(id);
            time += refresh_duration;
            return on_display
                ? on_display(id)
                : album::RotationDisplayResult::displayed;
        };

        result.now = [this] {
            events.push_back("now");
            if (now_failures > 0) {
                --now_failures;
                throw std::runtime_error("clock failed");
            }
            return time;
        };

        return result;
    }

    Result step()
    {
        events.clear();
        const auto all = callbacks();
        return album::run_rotation_step(state, all, random, stop);
    }

    void require_events(const std::vector<std::string>& expected) const
    {
        REQUIRE(events == expected);
    }
};

std::vector<std::string> display_events(album::ImageId id)
{
    return {"display:" + std::to_string(id), "now", "save"};
}

std::vector<std::string> concat(
    std::vector<std::string> a, const std::vector<std::string>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void first_start()
{
    Harness h;
    h.images = {3, 1, 2}; // Caller order is irrelevant; new IDs sort.

    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"load", "list", "now", "save"}, display_events(1)));
    REQUIRE(h.displays == std::vector<album::ImageId>{1});

    // The reconciled queue is saved before the panel is touched.
    REQUIRE(h.saves.size() == 2);
    require_same(h.saves[0], make_state(
        {1, 2, 3}, {1, 2, 3}, {}, std::nullopt, std::nullopt));

    // Deadline is measured from refresh completion, not selection.
    const auto expected = make_state(
        {1, 2, 3}, {2, 3}, {1}, 0, start + refresh_duration + 1h);
    require_same(h.saves[1], expected);
    REQUIRE(h.state.rotation);
    require_same(*h.state.rotation, expected);
    REQUIRE(!h.state.save_pending);
    REQUIRE(!h.state.completion_pending);

    // Loaded once; later steps use the in-memory state.
    REQUIRE(h.step() == Result::waiting);
    h.require_events({"list", "now"});
    REQUIRE(h.saves.size() == 2);
}

void first_start_empty()
{
    Harness h;

    // First start always creates the state file, even for an empty album.
    REQUIRE(h.step() == Result::idle);
    h.require_events({"load", "list", "now", "save"});
    REQUIRE(h.saves.size() == 1);
    require_same(h.saves[0], album::RotationState{});
    REQUIRE(h.displays.empty());
    REQUIRE(!h.state.save_pending);

    REQUIRE(h.step() == Result::idle);
    h.require_events({"list", "now"});
    REQUIRE(h.saves.size() == 1);
}

void waiting()
{
    Harness h;
    h.images = {1, 2};
    h.stored = make_state({1, 2}, {2}, {1}, 0, start + 10min);

    REQUIRE(h.step() == Result::waiting);
    h.require_events({"load", "list", "now"});

    // Unchanged state is never re-saved while waiting.
    h.time = start + 10min - 1s;
    REQUIRE(h.step() == Result::waiting);
    h.require_events({"list", "now"});
    REQUIRE(h.saves.empty());
    REQUIRE(h.displays.empty());

    // Exactly at the deadline, the change is due.
    h.time = start + 10min;
    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"list", "now"}, display_events(2)));
    REQUIRE(h.saves.size() == 1);
    require_same(h.saves[0], make_state(
        {1, 2}, {}, {1, 2}, 1, start + 10min + refresh_duration + 1h));
}

void overdue()
{
    Harness h;
    h.images = {1, 2, 3};
    auto saved = make_state({1, 2, 3}, {2, 3}, {1}, 0, start - 5h);
    saved.settings.interval = 45min;
    h.stored = saved;

    // Several missed intervals still produce only one refresh.
    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"load", "list", "now"}, display_events(2)));
    REQUIRE(h.displays == std::vector<album::ImageId>{2});
    REQUIRE(h.state.rotation->next_change
        == start + refresh_duration + 45min);

    REQUIRE(h.step() == Result::waiting);
    h.require_events({"list", "now"});
    REQUIRE(h.displays.size() == 1);
}

void new_cycle()
{
    Harness h;
    h.images = {1, 2};
    h.stored = make_state({1, 2}, {}, {1, 2}, 1, start - 1s);

    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"load", "list", "now", "save"}, display_events(1)));
    REQUIRE(h.saves.size() == 2);
    require_same(h.saves[0], make_state({1, 2}, {1, 2}, {1, 2}, 1, start - 1s));
    require_same(h.saves[1], make_state(
        {1, 2}, {2}, {1, 2, 1}, 2, start + refresh_duration + 1h));
}

void shuffle_saved()
{
    for (unsigned seed = 1; seed <= 20; ++seed) {
        Harness h;
        h.random.seed(seed);
        h.images = {1, 2, 3, 4};
        auto saved = make_state({1, 2, 3, 4}, {}, {1, 2, 3, 4}, 3, start - 1min);
        saved.settings.shuffle = true;
        h.stored = saved;

        REQUIRE(h.step() == Result::displayed);
        REQUIRE(h.saves.size() == 2);
        REQUIRE(h.displays.size() == 1);

        // The generated permutation is persisted before display.
        const auto queue = h.saves[0].remaining;
        auto sorted = queue;
        std::sort(sorted.begin(), sorted.end());
        REQUIRE(sorted == (std::vector<album::ImageId>{1, 2, 3, 4}));
        REQUIRE(queue.front() != 4); // No repeat across the cycle boundary.
        h.require_events(concat(
            {"load", "list", "now", "save"}, display_events(queue.front())));

        // The displayed image is the saved front; the rest stays in order.
        REQUIRE(h.displays[0] == queue.front());
        REQUIRE(h.saves[1].remaining
            == std::vector<album::ImageId>(queue.begin() + 1, queue.end()));
        REQUIRE(h.saves[1].history
            == (std::vector<album::ImageId>{1, 2, 3, 4, queue.front()}));
        REQUIRE(h.saves[1].order == (std::vector<album::ImageId>{1, 2, 3, 4}));
        REQUIRE(h.saves[1].settings.shuffle);
    }
}

void forward_history()
{
    Harness h;
    h.images = {1, 2, 3};
    // The user went Previous from 2 to 1; forward history comes first.
    h.stored = make_state({1, 2, 3}, {3}, {1, 2}, 0, start);

    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"load", "list", "now"}, display_events(2)));
    require_same(h.saves[0], make_state(
        {1, 2, 3}, {3}, {1, 2}, 1, start + refresh_duration + 1h));
}

void empty_album()
{
    Harness h;
    h.stored = album::RotationState{};

    REQUIRE(h.step() == Result::idle);
    h.require_events({"load", "list", "now"});
    REQUIRE(h.saves.empty());

    // The first upload is shown on the next step.
    h.images = {7};
    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"list", "now", "save"}, display_events(7)));
    require_same(h.saves[0], make_state({7}, {7}, {}, std::nullopt, std::nullopt));
    require_same(h.saves[1], make_state(
        {7}, {}, {7}, 0, start + refresh_duration + 1h));
}

void single_image()
{
    Harness h;
    h.images = {5};

    REQUIRE(h.step() == Result::displayed);
    REQUIRE(h.displays == std::vector<album::ImageId>{5});
    REQUIRE(h.saves.size() == 2);

    // Even long after the deadline, one image is never refreshed again.
    h.time += 5h;
    REQUIRE(h.step() == Result::idle);
    h.require_events({"list", "now"});
    REQUIRE(h.displays.size() == 1);
    REQUIRE(h.saves.size() == 2);
}

void reconcile()
{
    Harness h;
    h.images = {1, 3, 4}; // 2 was deleted, 4 was uploaded.
    h.stored = make_state({1, 2, 3}, {2, 3}, {1}, 0, start + 10min);

    REQUIRE(h.step() == Result::waiting);
    h.require_events({"load", "list", "now", "save"});
    REQUIRE(h.displays.empty());
    REQUIRE(h.saves.size() == 1);
    require_same(h.saves[0], make_state(
        {1, 3, 4}, {3, 4}, {1}, 0, start + 10min));
    require_same(*h.state.rotation, h.saves[0]);

    // Shuffle mode inserts uploads into the remaining queue.
    Harness s;
    s.images = {1, 2, 3, 9};
    auto saved = make_state({1, 2, 3}, {2, 3}, {1}, 0, start + 10min);
    saved.settings.shuffle = true;
    s.stored = saved;

    REQUIRE(s.step() == Result::waiting);
    REQUIRE(s.saves.size() == 1);
    REQUIRE(s.saves[0].order == (std::vector<album::ImageId>{1, 2, 3, 9}));
    auto queue = s.saves[0].remaining;
    REQUIRE(queue.size() == 3);
    REQUIRE(std::find(queue.begin(), queue.end(), 9) != queue.end());
    queue.erase(std::find(queue.begin(), queue.end(), 9));
    REQUIRE(queue == (std::vector<album::ImageId>{2, 3}));
}

void deleted_current()
{
    Harness h;
    h.images = {2}; // The image on screen (1) was deleted.
    h.stored = make_state({1, 2}, {2}, {1}, 0, start + 10min);

    // Deletion waits for the scheduled change instead of refreshing early.
    REQUIRE(h.step() == Result::waiting);
    h.require_events({"load", "list", "now", "save"});
    REQUIRE(h.displays.empty());
    require_same(h.saves[0], make_state({2}, {2}, {1}, 0, start + 10min));

    h.time = start + 10min;
    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"list", "now"}, display_events(2)));

    h.time += 2h;
    REQUIRE(h.step() == Result::idle);
    REQUIRE(h.displays.size() == 1);
}

void save_before_display()
{
    Harness h;
    h.images = {1, 2};
    h.save_failures = 1;

    // A failed pre-display save prevents the refresh.
    expect_throw<std::runtime_error>([&] { h.step(); });
    h.require_events({"load", "list", "now", "save"});
    REQUIRE(h.displays.empty());
    REQUIRE(h.saves.empty());
    REQUIRE(h.state.rotation);
    REQUIRE(h.state.save_pending);
    REQUIRE(!h.state.completion_pending);
    require_same(*h.state.rotation, make_state(
        {1, 2}, {1, 2}, {}, std::nullopt, std::nullopt));

    // Retry flushes first, does not reload, then displays.
    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"save", "list", "now"}, display_events(1)));
    REQUIRE(h.saves.size() == 2);
    require_same(h.saves[0], make_state(
        {1, 2}, {1, 2}, {}, std::nullopt, std::nullopt));
}

void display_failure()
{
    Harness h;
    h.images = {1, 2, 3, 4};
    auto saved = make_state({1, 2, 3, 4}, {}, {4}, 0, start - 1s);
    saved.settings.shuffle = true;
    h.stored = saved;
    h.on_display = [](album::ImageId) -> album::RotationDisplayResult {
        throw std::runtime_error("panel failed");
    };

    expect_throw<std::runtime_error>([&] { h.step(); });
    REQUIRE(h.displays.size() == 1);
    REQUIRE(h.saves.size() == 1);
    const auto queue = h.saves[0].remaining;
    REQUIRE(queue.size() == 4);
    REQUIRE(h.displays[0] == queue.front());

    // The target stays queued and history is unchanged.
    require_same(*h.state.rotation, h.saves[0]);
    REQUIRE(h.state.rotation->history == std::vector<album::ImageId>{4});
    REQUIRE(!h.state.save_pending);
    REQUIRE(!h.state.completion_pending);

    // Retry shows the same image without reshuffling or re-saving the queue.
    h.on_display = nullptr;
    REQUIRE(h.step() == Result::displayed);
    h.require_events(concat({"list", "now"}, display_events(queue.front())));
    REQUIRE(h.displays == (std::vector<album::ImageId>{queue.front(), queue.front()}));
    REQUIRE(h.saves.size() == 2);
    REQUIRE(h.saves[1].remaining
        == std::vector<album::ImageId>(queue.begin() + 1, queue.end()));
}

void save_after_display()
{
    Harness h;
    h.images = {1, 2};
    h.stored = make_state({1, 2}, {1, 2}, {}, std::nullopt, std::nullopt);
    h.on_display = [&h](album::ImageId) {
        h.save_failures = 1;
        return album::RotationDisplayResult::displayed;
    };

    expect_throw<std::runtime_error>([&] { h.step(); });
    h.require_events(concat({"load", "list", "now"}, display_events(1)));
    REQUIRE(h.saves.empty());

    // The refresh is recorded in memory; only the save is outstanding.
    const auto expected = make_state(
        {1, 2}, {2}, {1}, 0, start + refresh_duration + 1h);
    require_same(*h.state.rotation, expected);
    REQUIRE(h.state.save_pending);
    REQUIRE(!h.state.completion_pending);

    // A pending save is flushed even during shutdown, without a refresh.
    h.on_display = nullptr;
    h.stop = true;
    REQUIRE(h.step() == Result::stopped);
    h.require_events({"save"});
    REQUIRE(h.saves.size() == 1);
    require_same(h.saves[0], expected);
    REQUIRE(!h.state.save_pending);

    h.stop = false;
    REQUIRE(h.step() == Result::waiting);
    h.require_events({"list", "now"});
    REQUIRE(h.displays.size() == 1);

    // Repeated save failures never repeat the refresh.
    Harness r;
    r.images = {1, 2};
    r.stored = make_state({1, 2}, {1, 2}, {}, std::nullopt, std::nullopt);
    r.on_display = [&r](album::ImageId) {
        r.save_failures = 3;
        return album::RotationDisplayResult::displayed;
    };
    expect_throw<std::runtime_error>([&] { r.step(); });
    r.on_display = nullptr;
    r.time += 2h; // Even if the new deadline has passed meanwhile.
    for (int attempt = 0; attempt < 2; ++attempt) {
        expect_throw<std::runtime_error>([&] { r.step(); });
        r.require_events({"save"});
    }
    REQUIRE(r.displays.size() == 1);
    REQUIRE(r.step() == Result::displayed);
    REQUIRE(r.displays == (std::vector<album::ImageId>{1, 2}));
}

void clock_after_display()
{
    Harness h;
    h.images = {1, 2};
    h.stored = make_state({1, 2}, {1, 2}, {}, std::nullopt, std::nullopt);
    h.on_display = [&h](album::ImageId) {
        h.now_failures = 1;
        return album::RotationDisplayResult::displayed;
    };

    expect_throw<std::runtime_error>([&] { h.step(); });
    h.require_events({"load", "list", "now", "display:1", "now"});
    REQUIRE(h.saves.empty());
    REQUIRE(h.state.completion_pending);
    REQUIRE(h.state.save_pending);
    REQUIRE(h.state.rotation->history == std::vector<album::ImageId>{1});
    REQUIRE(h.state.rotation->remaining == std::vector<album::ImageId>{2});

    // Retry records completion at the retry time, then saves, then waits.
    h.on_display = nullptr;
    h.time = start + 10min;
    REQUIRE(h.step() == Result::waiting);
    h.require_events({"now", "save", "list", "now"});
    REQUIRE(!h.state.completion_pending);
    REQUIRE(!h.state.save_pending);
    REQUIRE(h.saves.size() == 1);
    require_same(h.saves[0], make_state({1, 2}, {2}, {1}, 0, start + 10min + 1h));
    REQUIRE(h.displays.size() == 1);

    // Completion is recorded before a pending flush honors shutdown.
    Harness s;
    s.images = {1, 2};
    s.stored = make_state({1, 2}, {1, 2}, {}, std::nullopt, std::nullopt);
    s.on_display = [&s](album::ImageId) {
        s.now_failures = 1;
        return album::RotationDisplayResult::displayed;
    };
    expect_throw<std::runtime_error>([&] { s.step(); });
    s.stop = true;
    REQUIRE(s.step() == Result::stopped);
    s.require_events({"now", "save"});
    REQUIRE(s.saves.size() == 1);
    REQUIRE(s.displays.size() == 1);
}

void stop()
{
    // Before loading: no callbacks at all.
    {
        Harness h;
        h.images = {1};
        h.stop = true;
        REQUIRE(h.step() == Result::stopped);
        h.require_events({});
        REQUIRE(!h.state.rotation);
    }

    // During the pre-display save: saved, but no refresh begins.
    {
        Harness h;
        h.images = {1, 2};
        h.on_save = [&h] { h.stop = true; };
        REQUIRE(h.step() == Result::stopped);
        h.require_events({"load", "list", "now", "save"});
        REQUIRE(h.displays.empty());
        REQUIRE(h.saves.size() == 1);
        REQUIRE(!h.state.save_pending);
    }

    // Cancelled before the refresh starts: state is unchanged.
    {
        Harness h;
        h.images = {1, 2};
        h.on_display = [&h](album::ImageId) {
            h.stop = true;
            return album::RotationDisplayResult::cancelled;
        };
        REQUIRE(h.step() == Result::stopped);
        REQUIRE(h.displays.size() == 1);
        REQUIRE(h.saves.size() == 1);
        require_same(*h.state.rotation, h.saves[0]);
        REQUIRE(h.state.rotation->history.empty());
        REQUIRE(!h.state.save_pending);
        REQUIRE(!h.state.completion_pending);
    }

    // Stop arriving during a refresh that completes: it is still recorded.
    {
        Harness h;
        h.images = {1, 2};
        h.on_display = [&h](album::ImageId) {
            h.stop = true;
            return album::RotationDisplayResult::displayed;
        };
        REQUIRE(h.step() == Result::displayed);
        h.require_events(concat({"load", "list", "now", "save"}, display_events(1)));
        REQUIRE(h.saves.size() == 2);
        REQUIRE(h.saves[1].history == std::vector<album::ImageId>{1});

        REQUIRE(h.step() == Result::stopped);
        h.require_events({});
    }
}

void invalid_display_result()
{
    const auto check = [](album::RotationDisplayResult result) {
        Harness h;
        h.images = {1, 2};
        h.on_display = [result](album::ImageId) { return result; };

        expect_throw<std::runtime_error>([&] { h.step(); });
        REQUIRE(h.displays.size() == 1);
        REQUIRE(h.saves.size() == 1);
        require_same(*h.state.rotation, h.saves[0]);
        REQUIRE(h.state.rotation->history.empty());
        REQUIRE(!h.state.save_pending);
        REQUIRE(!h.state.completion_pending);
    };

    // Cancelled is only valid during shutdown.
    check(album::RotationDisplayResult::cancelled);
    check(static_cast<album::RotationDisplayResult>(42));
}

void missing_callbacks()
{
    for (int missing = 0; missing < 5; ++missing) {
        Harness h;
        h.images = {1};
        auto all = h.callbacks();
        switch (missing) {
        case 0: all.list_images = nullptr; break;
        case 1: all.load_state = nullptr; break;
        case 2: all.save_state = nullptr; break;
        case 3: all.display = nullptr; break;
        default: all.now = nullptr; break;
        }

        expect_throw<std::invalid_argument>([&] {
            album::run_rotation_step(h.state, all, h.random, h.stop);
        });
        REQUIRE(h.events.empty());
        REQUIRE(!h.state.rotation);
    }
}

void invalid_state()
{
    // Save pending without state.
    {
        Harness h;
        h.state.save_pending = true;
        expect_throw<std::invalid_argument>([&] { h.step(); });
        h.require_events({});
    }

    // Completion pending without save pending.
    {
        Harness h;
        h.state.rotation = album::RotationState{};
        h.state.completion_pending = true;
        expect_throw<std::invalid_argument>([&] { h.step(); });
        h.require_events({});
    }

    // Invalid in-memory state is rejected before any pending save.
    {
        Harness h;
        album::RotationState bad;
        bad.settings.interval = 1min;
        h.state.rotation = bad;
        h.state.save_pending = true;
        h.state.completion_pending = true;
        expect_throw<std::invalid_argument>([&] { h.step(); });
        h.require_events({});
        REQUIRE(h.state.save_pending);
        REQUIRE(h.state.completion_pending);
    }

    // Invalid loaded state is not installed; the next step reloads.
    {
        Harness h;
        h.images = {1};
        h.stored = make_state({1, 1}, {}, {}, std::nullopt, std::nullopt);
        expect_throw<std::invalid_argument>([&] { h.step(); });
        h.require_events({"load"});
        REQUIRE(!h.state.rotation);
        REQUIRE(!h.state.save_pending);

        h.stored = make_state({1}, {1}, {}, std::nullopt, std::nullopt);
        REQUIRE(h.step() == Result::displayed);
        REQUIRE(h.events.front() == "load");
    }

    // Load failures propagate and leave the state unloaded.
    {
        Harness h;
        h.load_throws = true;
        expect_throw<std::runtime_error>([&] { h.step(); });
        h.require_events({"load"});
        REQUIRE(!h.state.rotation);
        REQUIRE(!h.state.save_pending);
    }

    // Invalid album listings leave the loaded state unchanged.
    for (const auto& listing : std::vector<std::vector<album::ImageId>>{
            {0}, {2, 2}}) {
        Harness h;
        h.images = listing;
        const auto saved = make_state({1, 2}, {2}, {1}, 0, start + 10min);
        h.stored = saved;
        expect_throw<std::invalid_argument>([&] { h.step(); });
        h.require_events({"load", "list"});
        require_same(*h.state.rotation, saved);
        REQUIRE(!h.state.save_pending);
        REQUIRE(h.displays.empty());
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::pair<std::string, void (*)()>> cases{
            {"first_start", first_start},
            {"first_start_empty", first_start_empty},
            {"waiting", waiting},
            {"overdue", overdue},
            {"new_cycle", new_cycle},
            {"shuffle_saved", shuffle_saved},
            {"forward_history", forward_history},
            {"empty_album", empty_album},
            {"single_image", single_image},
            {"reconcile", reconcile},
            {"deleted_current", deleted_current},
            {"save_before_display", save_before_display},
            {"display_failure", display_failure},
            {"save_after_display", save_after_display},
            {"clock_after_display", clock_after_display},
            {"stop", stop},
            {"invalid_display_result", invalid_display_result},
            {"missing_callbacks", missing_callbacks},
            {"invalid_state", invalid_state}
        };
        if (argc != 2) {
            throw std::runtime_error("Expected one rotation step test case name");
        }
        for (const auto& entry : cases) {
            if (entry.first == argv[1]) {
                entry.second();
                return 0;
            }
        }
        throw std::runtime_error("Unknown rotation step test case");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
