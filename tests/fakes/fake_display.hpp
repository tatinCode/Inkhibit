#ifndef INKHIBIT_TESTS_FAKE_DISPLAY_HPP
#define INKHIBIT_TESTS_FAKE_DISPLAY_HPP

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <vector>

// Test-only replacement for inkhibit::Display and inkhibit::DisplayGuard.
// Records sessions and follows a script instead of touching the panel or
// /var/lib/inkhibit.
namespace fake_display {

    enum class Outcome {
        success,
        cooldown,   // show() throws DisplayCooldown(cooldown_ms)
        failure     // show() throws std::runtime_error
    };

    struct Step {
        Outcome outcome = Outcome::success;
        std::uint64_t cooldown_ms = 0;
    };

    struct Controls {
        // Outcomes for successive show() calls; empty means success.
        std::deque<Step> steps;

        // Optional hooks, run before the scripted outcome.
        std::function<void()> on_construct;
        std::function<void()> on_show;

        std::vector<std::filesystem::path> state_directories;
        int destroyed = 0;
        int show_calls = 0;

        // Frames from successful show() calls only.
        std::vector<std::vector<std::uint8_t>> shown;
    };

    Controls& controls();

} // namespace fake_display

#endif
