#ifndef INKHIBIT_ALBUM_ROTATION_STORE_HPP
#define INKHIBIT_ALBUM_ROTATION_STORE_HPP

#include "inkhibit/album/rotation.hpp"

#include <filesystem>
#include <optional>

namespace inkhibit::album {

    // Returns nullopt only when .rotation-state does not exist.
    // Malformed data and I/O failures throw.
    std::optional<RotationState> load_rotation_state(
            const std::filesystem::path& album_root);

    // Validates state, writes .rotation-state.tmp, then renames it.
    // The album directory must already exist.
    // Only the rotation worker may read/write this state file.
    void save_rotation_state(
            const std::filesystem::path& album_root,
            const RotationState& state);

} // namespace inkhibit::album

#endif
