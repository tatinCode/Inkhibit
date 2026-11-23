#ifndef INKHIBIT_DISPLAY_COOLDOWN_HPP
#define INKHIBIT_DISPLAY_COOLDOWN_HPP

#include <cstdint>
#include <string>
#include <stdexcept>

namespace inkhibit{

    class DisplayCooldown : public std::runtime_error {
        public:
            DisplayCooldown(std::uint64_t remaining_ms, const std::string& message);

            std::uint64_t remaining_ms() const noexcept;

        private:
            std::uint64_t _remaining_ms;
    };
}

#endif
