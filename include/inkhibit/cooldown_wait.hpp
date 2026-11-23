#ifndef INKHIBIT_COOLDOWN_WAIT_HPP
#define INKHIBIT_COOLDOWN_WAIT_HPP

#include <csignal>
#include <cstdint>

namespace inkhibit{
    // Returns true when the wait completes, or false if a signal was requested.
    bool wait_for_cooldown(
            std::uint64_t remaining_ms,
            const volatile std::sig_atomic_t& requested_signal);

}   //end of namespace inkhibit

#endif
