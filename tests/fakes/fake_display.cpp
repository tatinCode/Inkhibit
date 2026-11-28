#include "fake_display.hpp"

#include "inkhibit/display.hpp"
#include "inkhibit/display_cooldown.hpp"
#include "inkhibit/display_guard.hpp"

#include <stdexcept>

namespace fake_display {

    Controls& controls()
    {
        static Controls instance;
        return instance;
    }

} // namespace fake_display

namespace inkhibit {

    // Never creates, opens, or locks the state directory.
    DisplayGuard::DisplayGuard(const std::filesystem::path&) {}
    DisplayGuard::~DisplayGuard() noexcept {}
    void DisplayGuard::begin() {}
    void DisplayGuard::complete() {}

    Display::Display(const std::filesystem::path& state_directory)
        : guard_(state_directory)
    {
        auto& fake = fake_display::controls();
        fake.state_directories.push_back(state_directory);
        if (fake.on_construct) {
            fake.on_construct();
        }
    }

    Display::~Display() noexcept
    {
        ++fake_display::controls().destroyed;
    }

    void Display::show(const std::vector<std::uint8_t>& framebuffer)
    {
        auto& fake = fake_display::controls();
        ++fake.show_calls;

        if (fake.on_show) {
            fake.on_show();
        }

        fake_display::Step step;
        if (!fake.steps.empty()) {
            step = fake.steps.front();
            fake.steps.pop_front();
        }

        switch (step.outcome) {
        case fake_display::Outcome::cooldown:
            throw DisplayCooldown(step.cooldown_ms, "Display cooldown active");
        case fake_display::Outcome::failure:
            throw std::runtime_error("Displaying framebuffer: fake failure");
        case fake_display::Outcome::success:
            break;
        }

        fake.shown.push_back(framebuffer);
    }

    void Display::close() {}

    void Display::shutdown_noexcept() noexcept {}

} // namespace inkhibit
