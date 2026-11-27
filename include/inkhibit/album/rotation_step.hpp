#ifndef INKHIBIT_ALBUM_ROTATION_STEP_HPP
#define INKHIBIT_ALBUM_ROTATION_STEP_HPP

#include "inkhibit/album/rotation.hpp"
#include "inkhibit/album/rotation_worker.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <random>
#include <vector>

namespace inkhibit::album {

    struct RotationStepCallbacks {
        std::function<std::vector<ImageId>()> list_images;

        std::function<std::optional<RotationState>()> load_state;

        std::function<void(const RotationState&)> save_state;

        RotationDisplay display;

        std::function<std::chrono::system_clock::time_point()> now; 
    };

    struct RotationStepState {
        // Empty until initial loading succeeds.
        std::optional<RotationState> rotation;

        // While true, saving must succeed before another display attempt.
        // In particular, a failed save after refresh must not repeat it.
        bool save_pending = false;

        // Display succeeded, but its completion deadline still needs recording.
        // Retry this before saving or attempting another display.
        bool completion_pending = false;
    };

    enum class RotationStepResult {
        idle,       // No automatic refresh needed, e.g. empty/single-image album.
        waiting,    // An image change is scheduled for later.
        displayed,  // One image was successfully displayed and state saved.
        stopped     // Shutdown requested; no further refresh should begin.
    };

    // Called only by the rotation worker, never concurrently.
    //
    // Performs at most one display attempt and never sleeps.
    // Storage/display failures throw; the caller applies retry backoff.
    // Retain this state object across retries.
    //
    // If display succeeds but saving fails, the updated rotation state must
    // remain in memory with save_pending set.
    RotationStepResult run_rotation_step(
            RotationStepState& state,
            const RotationStepCallbacks& callbacks,
            std::mt19937& random,
            const std::atomic<bool>& stop_requested);

} // namespace inkhibit::album

#endif
