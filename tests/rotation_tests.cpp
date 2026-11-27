#include "inkhibit/album/rotation.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace album = inkhibit::album;
using Ids = std::vector<album::ImageId>;
using Clock = std::chrono::system_clock;
using namespace std::chrono_literals;

#define REQUIRE(condition) do { if (!(condition)) { \
    throw std::runtime_error(std::string(__FILE__) + ":" \
        + std::to_string(__LINE__) + ": " + #condition); \
} } while (false)

template<class Function>
void expect_invalid(Function function)
{
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected std::invalid_argument");
}

void require_same(const album::RotationState& a, const album::RotationState& b)
{
    REQUIRE(a.settings.interval == b.settings.interval);
    REQUIRE(a.settings.shuffle == b.settings.shuffle);
    REQUIRE(a.order == b.order);
    REQUIRE(a.remaining == b.remaining);
    REQUIRE(a.history == b.history);
    REQUIRE(a.history_position == b.history_position);
    REQUIRE(a.next_change == b.next_change);
}

void require_target(const std::optional<album::RotationTarget>& target,
    album::ImageId id, std::optional<std::size_t> position = std::nullopt)
{
    REQUIRE(target);
    REQUIRE(target->id == id);
    REQUIRE(target->history_position == position);
}

album::RotationState example()
{
    album::RotationState state;
    state.order = {1, 2, 3, 4};
    state.remaining = {4};
    state.history = {1, 2, 3};
    state.history_position = 2;
    state.next_change = Clock::time_point{100h};
    return state;
}

void defaults()
{
    album::RotationState state;
    REQUIRE(state.settings.interval == 1h);
    REQUIRE(!state.settings.shuffle);
    REQUIRE(!state.history_position);
    REQUIRE(!state.next_change);
    album::validate_rotation_state(state);
    REQUIRE(!album::next_rotation_target(state));
    REQUIRE(!album::previous_rotation_target(state));
}

void intervals()
{
    for (const auto interval : {0s, -1s, 1799s}) {
        album::RotationState state;
        state.settings.interval = interval;
        expect_invalid([&] { album::validate_rotation_state(state); });
    }
    for (const auto interval : {1800s, 3600s, 86400s}) {
        album::RotationState state;
        state.settings.interval = interval;
        album::validate_rotation_state(state);
    }
}

void invalid_order()
{
    for (const auto& ids : {Ids{0}, Ids{1, 1}, Ids{1, 0, 2}}) {
        album::RotationState state;
        state.order = ids;
        expect_invalid([&] { album::validate_rotation_state(state); });
    }
    album::RotationState state;
    state.order = {std::numeric_limits<album::ImageId>::max()};
    album::validate_rotation_state(state);
}

void invalid_queue()
{
    for (const auto& ids : {Ids{0}, Ids{5}, Ids{1, 1}}) {
        auto state = example();
        state.remaining = ids;
        expect_invalid([&] { album::validate_rotation_state(state); });
    }
}

void invalid_history()
{
    auto state = example();
    state.history.push_back(0);
    expect_invalid([&] { album::validate_rotation_state(state); });
    state = example();
    state.history_position.reset();
    expect_invalid([&] { album::validate_rotation_state(state); });
    state = example();
    state.history_position = state.history.size();
    expect_invalid([&] { album::validate_rotation_state(state); });
    state = {};
    state.history_position = 0;
    expect_invalid([&] { album::validate_rotation_state(state); });

    // Repeated displays and deleted IDs are legitimate history.
    state = example();
    state.history = {99, 1, 1};
    album::validate_rotation_state(state);
}

void sequential_cycle()
{
    std::mt19937 random{1};
    album::RotationState state;
    state.order = {8, 2, 5}; // Custom order, not numeric sorting.
    album::prepare_rotation_cycle(state, random);
    REQUIRE(state.remaining == state.order);
    REQUIRE(state.history.empty());
    REQUIRE(!state.next_change);

    state.remaining.clear();
    state.history = {2};
    state.history_position = 0;
    album::prepare_rotation_cycle(state, random);
    REQUIRE(state.remaining == (Ids{5, 8, 2}));
    state.remaining.clear();
    state.history = {5};
    album::prepare_rotation_cycle(state, random);
    REQUIRE(state.remaining == state.order);

    state.remaining.clear();
    state.history = {99}; // Displayed image was deleted.
    album::prepare_rotation_cycle(state, random);
    REQUIRE(state.remaining == state.order);
}

void empty_single()
{
    std::mt19937 random{2};
    for (const bool shuffle : {false, true}) {
        album::RotationState state;
        state.settings.shuffle = shuffle;
        album::prepare_rotation_cycle(state, random);
        REQUIRE(state.remaining.empty());
        state.order = {7};
        album::prepare_rotation_cycle(state, random);
        require_target(album::next_rotation_target(state), 7);
        album::record_rotation_success(state,
            *album::next_rotation_target(state), Clock::time_point{});
        album::prepare_rotation_cycle(state, random);
        REQUIRE(state.remaining.empty());
        REQUIRE(!album::next_rotation_target(state));

        // A different single image must still be displayed.
        state.order = {8};
        album::prepare_rotation_cycle(state, random);
        require_target(album::next_rotation_target(state), 8);
    }
}

void preserved_queue()
{
    std::mt19937 random{3};
    for (const bool shuffle : {false, true}) {
        auto state = example();
        state.settings.shuffle = shuffle;
        const auto before = state;
        const auto generator_before = random;
        album::prepare_rotation_cycle(state, random);
        require_same(state, before);
        REQUIRE(random == generator_before);
    }
}

void shuffle_cycles()
{
    // Test properties, not a library-specific std::shuffle permutation.
    for (unsigned seed = 0; seed < 100; ++seed) {
        std::mt19937 random{seed};
        auto state = example();
        state.settings.shuffle = true;
        state.remaining.clear();
        const auto before = state;
        album::prepare_rotation_cycle(state, random);
        auto sorted = state.remaining;
        std::sort(sorted.begin(), sorted.end());
        REQUIRE(sorted == state.order);
        REQUIRE(state.remaining.front() != 3);
        REQUIRE(state.order == before.order);
        REQUIRE(state.history == before.history);
        REQUIRE(state.history_position == before.history_position);
        REQUIRE(state.next_change == before.next_change);
    }
}

void reshuffle()
{
    for (unsigned seed = 0; seed < 100; ++seed) {
        std::mt19937 random{seed};
        auto state = example();
        state.settings.shuffle = true;
        state.remaining = {3, 4};
        const auto before = state;
        album::reshuffle_rotation(state, random);
        REQUIRE(state.remaining == (Ids{4, 3}));
        state.remaining = before.remaining;
        require_same(state, before);
    }

    std::mt19937 random{4};
    auto state = example();
    const auto before = state;
    album::reshuffle_rotation(state, random);
    require_same(state, before); // Shuffle disabled.
    state.settings.shuffle = true;
    state.remaining.clear();
    album::reshuffle_rotation(state, random);
    REQUIRE(state.remaining.empty()); // Must not begin a new cycle.
    state.remaining = {4};
    album::reshuffle_rotation(state, random);
    REQUIRE(state.remaining == (Ids{4}));
}

void selection()
{
    auto state = example();
    const auto before = state;
    require_target(album::next_rotation_target(state), 4);
    require_target(album::previous_rotation_target(state), 2, 1);
    require_same(state, before);
    state.history_position = 0;
    require_target(album::next_rotation_target(state), 2, 1);
    REQUIRE(!album::previous_rotation_target(state));
}

void deleted_history()
{
    auto state = example();
    state.order = {1, 3, 4};
    require_target(album::previous_rotation_target(state), 1, 0);
    state.history_position = 0;
    require_target(album::next_rotation_target(state), 3, 2);

    state.history = {1, 1, 99, 1};
    state.history_position = 3;
    REQUIRE(!album::previous_rotation_target(state));
    state.history_position = 0;
    require_target(album::next_rotation_target(state), 4);
}

void success_queue()
{
    auto state = example();
    state.settings.interval = 30min;
    const auto completed = Clock::time_point{200h};
    album::record_rotation_success(state,
        *album::next_rotation_target(state), completed);
    REQUIRE(state.history == (Ids{1, 2, 3, 4}));
    REQUIRE(state.history_position == 3);
    REQUIRE(state.remaining.empty());
    REQUIRE(state.order == (Ids{1, 2, 3, 4}));
    REQUIRE(state.next_change == completed + 30min);
    album::validate_rotation_state(state);

    // First-ever display establishes history and a deadline.
    state = {};
    state.order = {8};
    state.remaining = {8};
    album::record_rotation_success(state,
        *album::next_rotation_target(state), completed);
    REQUIRE(state.history == (Ids{8}));
    REQUIRE(state.history_position == 0);
    REQUIRE(state.next_change == completed + 1h);
}

void success_navigation()
{
    auto state = example();
    const auto before = state;
    const auto completed = Clock::time_point{200h};
    album::record_rotation_success(state,
        *album::previous_rotation_target(state), completed);
    REQUIRE(state.history_position == 1);
    REQUIRE(state.history == before.history);
    REQUIRE(state.remaining == before.remaining);
    REQUIRE(state.next_change == completed + 1h);
    album::record_rotation_success(state,
        *album::next_rotation_target(state), completed + 2h);
    REQUIRE(state.history_position == 2);
    REQUIRE(state.history == before.history);
    REQUIRE(state.remaining == before.remaining);
    REQUIRE(state.next_change == completed + 3h);
}

void abandoned_history()
{
    auto state = example();
    state.history = {1, 99, 99};
    state.history_position = 0;
    album::record_rotation_success(state,
        *album::next_rotation_target(state), Clock::time_point{});
    REQUIRE(state.history == (Ids{1, 4}));
    REQUIRE(state.history_position == 1);
    REQUIRE(state.remaining.empty());
}

void rejected_target()
{
    for (const auto target : {
             album::RotationTarget{99, std::nullopt},
             album::RotationTarget{1, std::nullopt},
             album::RotationTarget{2, 0},
             album::RotationTarget{3, 2},
             album::RotationTarget{1, 0},
             album::RotationTarget{1, 999}}) {
        auto state = example();
        const auto before = state;
        expect_invalid([&] {
            album::record_rotation_success(state, target, Clock::time_point{});
        });
        require_same(state, before);
    }
}

void history_limit()
{
    auto state = example();
    state.history.assign(album::maximum_rotation_history, 1);
    state.history_position = state.history.size() - 1;
    album::record_rotation_success(state,
        *album::next_rotation_target(state), Clock::time_point{});
    REQUIRE(state.history.size() == album::maximum_rotation_history);
    REQUIRE(state.history.back() == 4);
    REQUIRE(state.history_position == state.history.size() - 1);
    album::validate_rotation_state(state);
}

void deadline_limits()
{
    auto state = example();
    const auto before = state;
    expect_invalid([&] {
        album::record_rotation_success(state,
            *album::next_rotation_target(state), Clock::time_point::max());
    });
    require_same(state, before);

    // The last representable deadline itself is allowed.
    const auto delay = std::chrono::duration_cast<Clock::duration>(
        state.settings.interval);
    album::record_rotation_success(state,
        *album::next_rotation_target(state), Clock::time_point::max() - delay);
    REQUIRE(state.next_change == Clock::time_point::max());

    // This platform's clock cannot represent this many whole seconds.
    if (std::chrono::duration_cast<std::chrono::seconds>(
            Clock::duration::max()) < std::chrono::seconds::max()) {
        state = example();
        state.settings.interval = std::chrono::seconds::max();
        const auto huge_before = state;
        expect_invalid([&] {
            album::record_rotation_success(state,
                *album::next_rotation_target(state), Clock::time_point{});
        });
        require_same(state, huge_before);
    }
}

void failed_display()
{
    auto state = example();
    const auto before = state;
    // A failed refresh simply does not call record_rotation_success.
    for (int attempt = 0; attempt < 3; ++attempt) {
        require_target(album::next_rotation_target(state), 4);
        require_same(state, before);
    }
}

void multiple_cycles()
{
    for (const bool shuffle : {false, true}) {
        std::mt19937 random{5};
        album::RotationState state;
        state.order = {1, 2, 3, 4};
        state.settings.shuffle = shuffle;
        std::optional<album::ImageId> last;
        auto now = Clock::time_point{};
        for (int cycle = 0; cycle < 4; ++cycle) {
            album::prepare_rotation_cycle(state, random);
            Ids displayed;
            while (!state.remaining.empty()) {
                const auto target = album::next_rotation_target(state);
                REQUIRE(target);
                REQUIRE(!target->history_position);
                REQUIRE(!last || target->id != *last);
                displayed.push_back(target->id);
                now += 1h;
                album::record_rotation_success(state, *target, now);
                last = target->id;
                album::validate_rotation_state(state);
            }
            if (shuffle) {
                std::sort(displayed.begin(), displayed.end());
            }
            REQUIRE(displayed == state.order);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::pair<std::string, void (*)()>> cases{
            {"defaults", defaults},
            {"intervals", intervals},
            {"invalid_order", invalid_order},
            {"invalid_queue", invalid_queue},
            {"invalid_history", invalid_history},
            {"sequential_cycle", sequential_cycle},
            {"empty_single", empty_single},
            {"preserved_queue", preserved_queue},
            {"shuffle_cycles", shuffle_cycles},
            {"reshuffle", reshuffle},
            {"selection", selection},
            {"deleted_history", deleted_history},
            {"success_queue", success_queue},
            {"success_navigation", success_navigation},
            {"abandoned_history", abandoned_history},
            {"rejected_target", rejected_target},
            {"history_limit", history_limit},
            {"deadline_limits", deadline_limits},
            {"failed_display", failed_display},
            {"multiple_cycles", multiple_cycles}
        };
        if (argc != 2) {
            throw std::runtime_error("Expected one rotation test case name");
        }
        for (const auto& entry : cases) {
            if (entry.first == argv[1]) {
                entry.second();
                return 0;
            }
        }
        throw std::runtime_error("Unknown rotation test case");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
