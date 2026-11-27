#include "inkhibit/album/rotation_step.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace inkhibit::album {

namespace {

using Clock = std::chrono::system_clock;

// Installing a prepared success state must not allocate after display.
static_assert(std::is_nothrow_assignable_v<
    std::optional<RotationState>&, RotationState&&>);

bool same_state(const RotationState& a, const RotationState& b)
{
    return a.settings.interval == b.settings.interval
        && a.settings.shuffle == b.settings.shuffle
        && a.order == b.order
        && a.remaining == b.remaining
        && a.history == b.history
        && a.history_position == b.history_position
        && a.next_change == b.next_change;
}

bool inactive(const RotationState& state)
{
    if (state.order.empty()) {
        return true;
    }

    return state.order.size() == 1
        && state.history_position
        && state.order.front()
            == state.history.at(*state.history_position);
}

Clock::time_point next_deadline(
    Clock::time_point completed,
    std::chrono::seconds interval)
{
    if (interval > std::chrono::duration_cast<std::chrono::seconds>(
            Clock::duration::max())) {
        throw std::invalid_argument("Rotation interval is too large");
    }

    const auto delay =
        std::chrono::duration_cast<Clock::duration>(interval);

    if (completed > Clock::time_point::max() - delay) {
        throw std::invalid_argument("Rotation deadline is out of range");
    }

    return completed + delay;
}

void flush_pending(
    RotationStepState& state,
    const RotationStepCallbacks& callbacks) {
    if (state.completion_pending) {
        // If reading time fails, retain the flag and retry later.
        const auto completed = callbacks.now();

        state.rotation->next_change = next_deadline(
            completed, state.rotation->settings.interval);

        state.completion_pending = false;
    }

    if (state.save_pending) {
        callbacks.save_state(*state.rotation);

        // Clear only after the save succeeds.
        state.save_pending = false;
    }
}

} // namespace

RotationStepResult run_rotation_step(
    RotationStepState& state,
    const RotationStepCallbacks& callbacks,
    std::mt19937& random,
    const std::atomic<bool>& stop_requested)
{
    if (!callbacks.list_images || !callbacks.load_state
        || !callbacks.save_state || !callbacks.display || !callbacks.now) {
        throw std::invalid_argument(
            "Rotation step requires all callbacks");
    }

    if ((state.save_pending && !state.rotation)
        || (state.completion_pending && !state.save_pending)) {
        throw std::invalid_argument(
            "Rotation step has inconsistent pending state");
    }

    // ensures invalid in-memory state is rejected before 
    // pending-completion or save callbacks
    if (state.rotation) {
        validate_rotation_state(*state.rotation);
    }

    // Finish an already-completed refresh even if shutdown was requested.
    // A failed save must block all further display attempts.
    flush_pending(state, callbacks);

    if (stop_requested.load()) {
        return RotationStepResult::stopped;
    }

    if (!state.rotation) {
        auto loaded = callbacks.load_state();
        const bool first_start = !loaded;

        if (!loaded) {
            loaded.emplace();
        }

        validate_rotation_state(*loaded);

        state.rotation = std::move(loaded);
        state.save_pending = first_start;
    }

    auto updated = *state.rotation;
    reconcile_rotation_images(updated, callbacks.list_images(), random);

    const auto now = callbacks.now();
    const bool due = rotation_change_due(updated, now);

    std::optional<RotationTarget> target;

    if (due) {
        // Follow forward history before generating a new cycle.
        target = next_rotation_target(updated);

        if (!target) {
            prepare_rotation_cycle(updated, random);
            target = next_rotation_target(updated);
        }
    }

    if (!same_state(updated, *state.rotation)) {
        state.rotation = std::move(updated);
        state.save_pending = true;
    }

    // Persist reconciliation and any newly generated shuffle queue
    // before touching the panel.
    flush_pending(state, callbacks);

    if (stop_requested.load()) {
        return RotationStepResult::stopped;
    }

    if (!due) {
        return inactive(*state.rotation)
            ? RotationStepResult::idle
            : RotationStepResult::waiting;
    }

    if (!target) {
        return RotationStepResult::idle;
    }

    // Perform allocations, history updates, and deadline checks before
    // display. The actual completion deadline is installed afterward.
    auto success = *state.rotation;
    record_rotation_success(success, *target, now);

    if (stop_requested.load()) {
        return RotationStepResult::stopped;
    }

    const auto result = callbacks.display(target->id, stop_requested);

    if (result == RotationDisplayResult::cancelled) {
        if (!stop_requested.load()) {
            throw std::runtime_error(
                "Rotation display cancelled without a stop request");
        }

        return RotationStepResult::stopped;
    }

    if (result != RotationDisplayResult::displayed) {
        throw std::runtime_error("Invalid rotation display result");
    }

    // No allocating operations between successful display and recording
    // in memory that this image has already been shown.
    state.rotation = std::move(success);
    state.save_pending = true;
    state.completion_pending = true;

    flush_pending(state, callbacks);

    return RotationStepResult::displayed;
}

} // namespace inkhibit::album
