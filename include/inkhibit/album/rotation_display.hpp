#ifndef INKHIBIT_ALBUM_ROTATION_DISPLAY_HPP
#define INKHIBIT_ALBUM_ROTATION_DISPLAY_HPP

#include "inkhibit/album/rotation_worker.hpp"

#include <atomic>
#include <filesystem>

namespace inkhibit::album {

    // Loads and displays one album image.
    // Honors cancellation before refresh, never during it.
    // Throws on image, storage, or hardware failure.
    RotationDisplayResult display_rotation_image(
        const std::filesystem::path& album_root,
        ImageId id,
        const std::atomic<bool>& stop_requested);

} // namespace inkhibit::album

#endif
