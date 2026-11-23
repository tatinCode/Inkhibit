#include "inkhibit/display_cooldown.hpp"


namespace inkhibit{

    DisplayCooldown::DisplayCooldown (
            std::uint64_t remaining_ms,
            const std::string& message
            ) : std::runtime_error(message),
    _remaining_ms(remaining_ms)
    {
    }

    std::uint64_t DisplayCooldown::remaining_ms() const noexcept{
        return _remaining_ms;
    }

}   //end of namespace inkhibit
