#ifndef INKHIBIT_ALBUM_STORE_HPP
#define INKHIBIT_ALBUM_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

namespace inkhibit::album {

using ImageId = std::uint64_t;

// Use one AlbumStore instance per private storage directory.
// Threads may share that instance; multiple writers must not open the same root.
class AlbumStore {
public:
    explicit AlbumStore(const std::filesystem::path& root);

    ImageId add(const std::vector<std::uint8_t>& png);
    std::vector<ImageId> list() const;
    bool remove(ImageId id);

private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
};

} // namespace inkhibit::album

#endif
