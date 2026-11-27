#ifndef INKHIBIT_ALBUM_ROTATION_HPP
#define INKHIBIT_ALBUM_ROTATION_HPP

#include "inkhibit/album/store.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>
#include <random>

namespace inkhibit::album {

    inline constexpr auto minimum_rotation_interval = std::chrono::minutes{30};
    inline constexpr auto default_rotation_interval = std::chrono::minutes{60};
    inline constexpr std::size_t maximum_rotation_history = 256;

    struct RotationSettings {
        std::chrono::seconds interval = default_rotation_interval;
        bool shuffle = false;
    };

    struct RotationTarget {
        ImageId id;

        // Set when navigating existing history.
        // Empty when selecting the front of the remaining queue.
        std::optional<std::size_t> history_position;
    };

    // Owned by the rotation worker. HTTP handlers must not access it directly.
    struct RotationState {
        RotationSettings settings;

        // User-defined order. Initially populated in numeric ID order.
        std::vector<ImageId> order;

        // Remaining images in this cycle, in playback order.
        // In shuffle mode, this is a saved random permutation.
        std::vector<ImageId> remaining;

        // Successfully displayed images, oldest first. Keeping history lets
        // Previous work even after a shuffle cycle has finished.
        std::vector<ImageId> history;

        // Position in history of the currently displayed image.
        // Empty until the first successful display.
        std::optional<std::size_t> history_position;

        // Absolute deadline so restarting does not reset the interval.
        // Empty when no automatic change is scheduled.
        std::optional<std::chrono::system_clock::time_point> next_change;
    };

    // Throws std::invalid_argument if the state is inconsistent.
    void validate_rotation_state(const RotationState& state);

    // Builds a cycle only when the remaining queue is empty.
    // Does not change display history or the deadline.
    void prepare_rotation_cycle(RotationState& state, std::mt19937& random);

    // Shuffles only the remaining queue when shuffle mode is enabled.
    // Does not start a new cycle or change history or the deadline.
    void reshuffle_rotation(RotationState& state, std::mt19937& random);

    // These functions do not modify state.
    // Prepare a cycle before selecting Next if the queue needs replenishing.
    std::optional<RotationTarget> next_rotation_target(
            const RotationState& state);

    std::optional<RotationTarget> previous_rotation_target(
            const RotationState& state);

    // Call only after the selected image was successfully displayed.
    // State must not change between selection and this call.
    void record_rotation_success(
            RotationState& state,
            const RotationTarget& target,
            std::chrono::system_clock::time_point completed_at);

    // Removes missing images from custom order and the remaining queue.
    // Preserves history and the deadline. Does not add new images.
    void remove_missing_rotation_images(
            RotationState& state,
            const std::vector<ImageId>& available);

    // Reconciles saved state with the current album contents.
    // Preserves history and the deadline.
    void reconcile_rotation_images(
            RotationState& state,
            const std::vector<ImageId>& available,
            std::mt19937& random);

    // Whether automatic rotation should attempt to select an image now.
    // Does not modify state, prepare a cycle, or perform a refresh.
    bool rotation_change_due(
            const RotationState& state,
            std::chrono::system_clock::time_point now);

} // namespace inkhibit::album

#endif
