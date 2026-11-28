#include "inkhibit/album/rotation_worker.hpp"

#include "inkhibit/album/rotation_step.hpp"
#include "inkhibit/album/rotation_store.hpp"

#include <stdexcept>
#include <utility>
#include <chrono>
#include <random>
#include <exception>

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

    void RotationWorker::run() {
        RotationStepState state;
        std::mt19937 random{std::random_device{}()};

        RotationStepCallbacks callbacks;

        callbacks.list_images = [this] {
            return store_.list();
        };

        callbacks.load_state = [this] {
            return load_rotation_state(album_root_);
        };

        callbacks.save_state = [this](const RotationState& rotation) {
            save_rotation_state(album_root_, rotation);
        };

        callbacks.display = display_;

        callbacks.now = [] {
            return std::chrono::system_clock::now();
        };

        for (;;) {
            // Clear notifications before inspecting the album. Changes arriving
            // during the step stay recorded and prevent a missed wake-up.
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                album_changed_ = false;
            }

            try {
                const auto result = run_rotation_step(
                        state, callbacks, random, stop_requested_);

                if (result == RotationStepResult::stopped) {
                    return;
                }

                if (result == RotationStepResult::displayed) {
                    // Re-evaluate: a single-image album should become idle,
                    // while a larger album should wait for its new deadline.
                    continue;
                }

                std::unique_lock<std::mutex> lock(mutex_);

                const auto wake_requested = [this] {
                    return stop_requested_.load() || album_changed_;
                };

                if (result == RotationStepResult::waiting) {
                    changed_.wait_until(
                            lock, state.rotation->next_change.value(),
                            wake_requested);
                } else {
                    changed_.wait(lock, wake_requested);
                }
            } catch (const std::exception& error) {
                report_error_(error.what());

                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait_for(
                        lock, std::chrono::seconds{5},
                        [this] { return stop_requested_.load(); });

                if (stop_requested_.load()) {
                    return;
                }
            } catch (...) {
                report_error_("Unknown rotation worker error");

                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait_for(
                        lock, std::chrono::seconds{5},
                        [this] { return stop_requested_.load(); });

                if (stop_requested_.load()) {
                    return;
                }
            }
        }
    }

} // namespace inkhibit::album
