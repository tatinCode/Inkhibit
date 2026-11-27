#include "inkhibit/album/rotation.hpp"

#include <stdexcept>
#include <algorithm>
#include <utility>
#include <unordered_set>

namespace inkhibit::album {

    namespace {  //anonymous namespace
        std::optional<ImageId> current_image(const RotationState& state) {
            if (!state.history_position) {
                return std::nullopt;
            }

            return state.history.at(*state.history_position);
        }

        void shuffle_queue(std::vector<ImageId>& queue,
                std::optional<ImageId> current, std::mt19937& random)
        {
            std::shuffle(queue.begin(), queue.end(), random);

            // Avoid repeating the displayed image across a cycle boundary.
            if (queue.size() > 1 && current && queue.front() == *current) {
                std::uniform_int_distribution<std::size_t> pick(1, queue.size() - 1);
                std::swap(queue.front(), queue[pick(random)]);
            }
        }

    }   //end of namespace


    void validate_rotation_state(const RotationState& state) {
        if (state.settings.interval < minimum_rotation_interval) {
            throw std::invalid_argument(
                    "Rotation interval must be at least 30 minutes");
        }

        std::unordered_set<ImageId> album_ids;
        for (const auto id : state.order) {
            if (id == 0 || !album_ids.insert(id).second) {
                throw std::invalid_argument(
                        "Rotation order must contain unique positive image IDs");
            }
        }

        std::unordered_set<ImageId> queued_ids;
        for (const auto id : state.remaining) {
            if (album_ids.count(id) == 0) {
                throw std::invalid_argument(
                        "Rotation queue contains an image outside the album order");
            }

            if (!queued_ids.insert(id).second) {
                throw std::invalid_argument(
                        "Rotation queue contains duplicate image IDs");
            }
        }

        for (const auto id : state.history) {
            if (id == 0) {
                throw std::invalid_argument(
                        "Rotation history contains an invalid image ID");
            }
        }

        if (state.history.empty()) {
            if (state.history_position) {
                throw std::invalid_argument(
                        "Empty rotation history cannot have a position");
            }
        } else {
            if (!state.history_position
                    || *state.history_position >= state.history.size()) {
                throw std::invalid_argument(
                        "Rotation history position is missing or out of range");
            }
        }
    }

    void prepare_rotation_cycle(RotationState& state, std::mt19937& random) {
        validate_rotation_state(state);

        if (!state.remaining.empty() || state.order.empty()) {
            return;
        }

        const auto current = current_image(state);

        // A single-image album should not refresh the same image repeatedly.
        if (state.order.size() == 1 && current
                && state.order.front() == *current) {
            return;
        }

        auto queue = state.order;

        if (state.settings.shuffle) {
            shuffle_queue(queue, current, random);
        } else if (current) {
            const auto found = std::find(queue.begin(), queue.end(), *current);

            if (found != queue.end()) {
                // Start after the current image, preserving custom order.
                auto next = found;
                ++next;
                std::rotate(queue.begin(), next, queue.end());
            }
        }

        state.remaining = std::move(queue);
    }

    void reshuffle_rotation(RotationState& state, std::mt19937& random) {
        validate_rotation_state(state);

        if (!state.settings.shuffle) {
            return;
        }

        auto queue = state.remaining;
        shuffle_queue(queue, current_image(state), random);
        state.remaining = std::move(queue);
    }   //end of reshuffle roatation

    std::optional<RotationTarget> next_rotation_target(
            const RotationState& state) {
        validate_rotation_state(state);

        // After Previous, follow existing history before consuming the queue.
        if (state.history_position) {
            for (auto position = *state.history_position + 1;
                    position < state.history.size(); ++position) {
                const auto id = state.history[position];

                // Deleted images remain in history but cannot be selected.
                if (std::find(state.order.begin(), state.order.end(), id)
                        != state.order.end()
                        && id != *current_image(state)) {
                    return RotationTarget{id, position};
                }
            }
        }

        if (state.remaining.empty()) {
            return std::nullopt;
        }

        return RotationTarget{state.remaining.front(), std::nullopt};
    }

    std::optional<RotationTarget> previous_rotation_target(
            const RotationState& state) {
        validate_rotation_state(state);

        if (!state.history_position) {
            return std::nullopt;
        }

        auto position = *state.history_position;

        // Check before decrementing to avoid unsigned underflow.
        while (position > 0) {
            --position;
            const auto id = state.history[position];

            if (std::find(state.order.begin(), state.order.end(), id)
                    != state.order.end()
                    && id != *current_image(state)) {
                return RotationTarget{id, position};
            }
        }

        return std::nullopt;
    }   //end of previous rotation target

    void record_rotation_success(
            RotationState& state,
            const RotationTarget& target,
            std::chrono::system_clock::time_point completed_at) {
        validate_rotation_state(state);

        const auto matches = [&target](
                const std::optional<RotationTarget>& candidate) {
            return candidate
                && candidate->id == target.id
                && candidate->history_position == target.history_position;
        };

        if (!matches(next_rotation_target(state))
                && !matches(previous_rotation_target(state))) {
            throw std::invalid_argument(
                    "Rotation target is no longer selectable");
        }

        using Clock = std::chrono::system_clock;

        // Check before converting seconds into the clock's finer duration.
        if (state.settings.interval
                > std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::duration::max())) {
            throw std::invalid_argument("Rotation interval is too large");
        }

        const auto delay =
            std::chrono::duration_cast<Clock::duration>(state.settings.interval);

        if (completed_at > Clock::time_point::max() - delay) {
            throw std::invalid_argument("Rotation deadline is out of range");
        }

        // Build a replacement so allocation failures leave state unchanged.
        auto updated = state;

        if (target.history_position) {
            // Previous or Next through existing history.
            updated.history_position = target.history_position;
        } else {
            // Queue selection: discard any abandoned forward history.
            if (updated.history_position) {
                updated.history.resize(*updated.history_position + 1);
            }

            updated.history.push_back(target.id);
            updated.remaining.erase(updated.remaining.begin());

            if (updated.history.size() > maximum_rotation_history) {
                const auto excess =
                    updated.history.size() - maximum_rotation_history;

                using Difference = std::vector<ImageId>::difference_type;
                updated.history.erase(
                        updated.history.begin(),
                        updated.history.begin() + static_cast<Difference>(excess));
            }

            updated.history_position = updated.history.size() - 1;
        }

        updated.next_change = completed_at + delay;
        state = std::move(updated);
    }   // end of record_rotation_success

} // end of namespace inkhibit::album




