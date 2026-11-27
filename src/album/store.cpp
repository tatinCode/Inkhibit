#include "inkhibit/album/store.hpp"

#include "inkhibit/album/image.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace inkhibit::album {
    namespace {

        namespace fs = std::filesystem;

        bool parse_id(std::string_view text, ImageId& id) {
            if (text.empty() || text.front() == '0') {
                return false;
            }

            const auto* end = text.data() + text.size();
            const auto result = std::from_chars(text.data(), end, id);
            return result.ec == std::errc{} && result.ptr == end;
        }

        fs::path image_path(const fs::path& root, ImageId id) {
            return root / (std::to_string(id) + ".png");
        }

        ImageId read_next_id(const fs::path& path) {
            if (!fs::exists(path)) {
                return 1;
            }

            std::ifstream input(path);
            std::string text;
            ImageId id = 0;
            if (!std::getline(input, text) || !parse_id(text, id)) {
                throw std::runtime_error("Invalid album ID counter: " + path.string());
            }
            return id;
        }

        // Publish only after the whole file has been written and closed.
        // A leftover .tmp file is ignored by list() and overwritten on the next write.
        void publish_file(const fs::path& path, const char* data, std::size_t size) {
            auto temporary = path;
            temporary += ".tmp";

            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Cannot open album file: " + temporary.string());
            }

            output.write(data, static_cast<std::streamsize>(size));
            output.close();
            if (!output) {
                throw std::runtime_error("Cannot write album file: " + temporary.string());
            }

            fs::rename(temporary, path);
        }

    } // namespace

    AlbumStore::AlbumStore(const fs::path& root)
        : root_(fs::absolute(root)) {
        fs::create_directories(root_);
    }

    ImageId AlbumStore::add(const std::vector<std::uint8_t>& png) {
        validate_png(png);

        const std::lock_guard<std::mutex> lock(mutex_);
        const auto counter = root_ / ".next-id";
        const auto id = read_next_id(counter);
        if (id == std::numeric_limits<ImageId>::max()) {
            throw std::runtime_error("Album image IDs exhausted");
        }

        const auto path = image_path(root_, id);
        if (fs::exists(path)) {
            throw std::runtime_error("Album image ID already exists: " + path.string());
        }

        // Reserve the ID first. If saving the PNG fails, a gap is fine;
        // reusing an ID that may already identify an image is not.
        const auto next = std::to_string(id + 1) + '\n';
        publish_file(counter, next.data(), next.size());
        publish_file(path, reinterpret_cast<const char*>(png.data()), png.size());
        return id;
    }

    std::vector<ImageId> AlbumStore::list() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ImageId> ids;

        for (const auto& entry : fs::directory_iterator(root_)) {
            if (!fs::is_regular_file(entry.symlink_status())
                    || entry.path().extension() != ".png") {
                continue;
            }

            const auto stem = entry.path().stem().string();
            ImageId id = 0;
            if (parse_id(stem, id)) {
                ids.push_back(id);
            }
        }

        std::sort(ids.begin(), ids.end());
        return ids;
    }

    bool AlbumStore::remove(ImageId id) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto path = image_path(root_, id);
        if (id == 0 || !fs::is_regular_file(fs::symlink_status(path))) {
            return false;
        }
        return fs::remove(path);
    }

} // namespace inkhibit::album
  //
