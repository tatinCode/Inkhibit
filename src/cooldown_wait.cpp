#include "inkhibit/cooldown_wait.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace inkhibit {

    bool wait_for_cooldown(
            std::uint64_t remaining_ms,
            const volatile std::sig_atomic_t& requested_signal)
    {
        while (remaining_ms > 0 && requested_signal == 0) {
            const auto chunk_ms = std::min<std::uint64_t>(remaining_ms, 100);

            std::this_thread::sleep_for(
                    std::chrono::milliseconds{static_cast<int>(chunk_ms)});

            remaining_ms -= chunk_ms;
        }

        return requested_signal == 0;
    }

} // namespace inkhibit
