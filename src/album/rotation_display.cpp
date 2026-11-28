#include "inkhibit/album/rotation_display.hpp"

#include "inkhibit/display.hpp"
#include "inkhibit/display_cooldown.hpp"
#include "inkhibit/image.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace inkhibit::album {

RotationDisplayResult display_rotation_image(
    const std::filesystem::path& album_root,
    ImageId id,
    const std::atomic<bool>& stop_requested)
{
    if (stop_requested.load()) {
        return RotationDisplayResult::cancelled;
    }

    if (id == 0) {
        throw std::invalid_argument("Invalid rotation image ID");
    }

    const auto path = album_root / (std::to_string(id) + ".png");
    const auto decoded = image::decode_image(path);
    const auto fitted = image::fit_to_canvas(decoded);
    const auto frame = image::quantize_to_framebuffer(fitted);

    while (!stop_requested.load()) {
        try {
            Display display;

            if (stop_requested.load()) {
                return RotationDisplayResult::cancelled;
            }

            // Once started, refresh and cleanup must finish uninterrupted.
            display.show(frame);

            return RotationDisplayResult::displayed;
        } catch (const DisplayCooldown& cooldown) {
            auto remaining = cooldown.remaining_ms();

            // Short chunks keep shutdown responsive and avoid converting
            // an arbitrarily large millisecond count to a clock duration.
            while (remaining > 0 && !stop_requested.load()) {
                const auto chunk =
                    std::min<std::uint64_t>(remaining, 100);

                std::this_thread::sleep_for(
                    std::chrono::milliseconds{chunk});

                remaining -= chunk;
            }

            // Construct a fresh Display and recheck the real guard.
        }
    }

    return RotationDisplayResult::cancelled;
}

} // namespace inkhibit::album
