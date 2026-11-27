#ifndef INKHIBIT_ALBUM_ROTATION_WORKER_HPP
#define INKHIBIT_ALBUM_ROTATION_WORKER_HPP

#include "inkhibit/album/store.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace inkhibit::album {

    enum class RotationDisplayResult {
        displayed,
        cancelled
    };

    // May throw on failure.
    // Cancellation is allowed before refresh starts, never during refresh.
    // Return displayed only after a successful refresh.
    using RotationDisplay = std::function<RotationDisplayResult(
            ImageId id,
            const std::atomic<bool>& stop_requested)>;

    // Receives recoverable errors. Must not throw.
    using RotationErrorHandler = std::function<void(const std::string&)>;

    class RotationWorker {
        public:
            RotationWorker(
                    AlbumStore& store,
                    const std::filesystem::path& album_root,
                    RotationDisplay display,
                    RotationErrorHandler report_error);

            RotationWorker(const RotationWorker&) = delete;
            RotationWorker& operator=(const RotationWorker&) = delete;

            // Blocking entry point: call exactly once, on a dedicated thread.
            // The caller must join that thread before destroying this object,
            // the AlbumStore, or anything captured by the callbacks.
            void run();

            // Thread-safe. Wakes the worker and cancels waiting, not an active refresh.
            void request_stop();

            // Thread-safe. Call after a successful album upload or deletion.
            void notify_album_changed();

        private:
            AlbumStore& store_;
            std::filesystem::path album_root_;
            RotationDisplay display_;
            RotationErrorHandler report_error_;

            std::atomic<bool> stop_requested_{false};
            std::mutex mutex_;
            std::condition_variable changed_;
            bool album_changed_ = false;
    };

} // namespace inkhibit::album

#endif
