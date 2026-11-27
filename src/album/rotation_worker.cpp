#include "inkhibit/album/rotation_worker.hpp"

#include <stdexcept>
#include <utility>

namespace inkhibit::album {

    RotationWorker::RotationWorker(
            AlbumStore& store,
            const std::filesystem::path& album_root,
            RotationDisplay display,
            RotationErrorHandler report_error)
        : store_(store),
        album_root_(std::filesystem::absolute(album_root)),
        display_(std::move(display)),
        report_error_(std::move(report_error)){
        if (!display_) {
            throw std::invalid_argument(
                    "Rotation worker requires a display callback");
        }

        if (!report_error_) {
            throw std::invalid_argument(
                    "Rotation worker requires an error handler");
        }
    }

    void RotationWorker::request_stop(){
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_.store(true);
        }

        changed_.notify_one();
    }

    void RotationWorker::notify_album_changed(){
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            album_changed_ = true;
        }

        changed_.notify_one();
    }

} // namespace inkhibit::album
