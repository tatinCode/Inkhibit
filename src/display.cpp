#include "inkhibit/display.hpp"
#include "inkhibit/driver_status.h"

extern "C" {
#include "EPD_4in0e.h"
}

#include <stdexcept>
#include <string>

namespace {

void check_driver(const char* operation)
{
    if (inkhibit_driver_error() == 0) {
        return;
    }

    throw std::runtime_error(
        std::string(operation)
        + ": "
        + inkhibit_driver_error_message()
    );
}

bool valid_color(std::uint8_t color)
{
    return color <= 3 || color == 5 || color == 6;
}

void validate_framebuffer(
    const std::vector<std::uint8_t>& framebuffer)
{
    if (framebuffer.size() != inkhibit::Display::framebuffer_size) {
        throw std::invalid_argument(
            "Framebuffer must contain exactly 120000 bytes"
        );
    }

    for (const std::uint8_t byte : framebuffer) {
        if (!valid_color(byte >> 4) || !valid_color(byte & 0x0F)) {
            throw std::invalid_argument(
                "Framebuffer contains an unsupported color code"
            );
        }
    }
}

} // namespace

namespace inkhibit {

static_assert(Display::native_width == EPD_4IN0E_WIDTH);
static_assert(Display::native_height == EPD_4IN0E_HEIGHT);

Display::Display(const std::filesystem::path& state_directory)
    : guard_(state_directory)
{
}

Display::~Display() noexcept
{
    shutdown_noexcept();
}

void Display::show(
    const std::vector<std::uint8_t>& framebuffer)
{
    if (attempted_) {
        throw std::logic_error(
            "A Display object supports one update; create a new session"
        );
    }

    validate_framebuffer(framebuffer);
    attempted_ = true;

    try {
        guard_.begin();

        if (DEV_Module_Init() != 0) {
            check_driver("Initializing GPIO/SPI");

            throw std::runtime_error(
                "GPIO/SPI initialization failed without a diagnostic"
            );
        }

        module_open_ = true;

        EPD_4IN0E_Init();
        check_driver("Initializing panel");
        panel_ready_ = true;

        /*
         * The official function is not const-correct, but its verified
         * implementation only reads the supplied framebuffer.
         */
        EPD_4IN0E_Display(
            const_cast<std::uint8_t*>(framebuffer.data())
        );

        check_driver("Displaying framebuffer");
        close();
    } catch (...) {
        shutdown_noexcept();

        /*
         * Leave the active marker after failure. A later session will
         * start a conservative recovery cooldown.
         */
        throw;
    }
}

void Display::close()
{
    if (!module_open_) {
        return;
    }

    if (panel_ready_ && inkhibit_driver_error() == 0) {
        /*
         * The checked C driver already includes the two-second delay
         * after its deep-sleep command.
         */
        EPD_4IN0E_Sleep();
    }

    // Always attempt resource release, including after sleep failure.
    DEV_Module_Exit();

    module_open_ = false;
    panel_ready_ = false;

    check_driver("Shutting down display");
    guard_.complete();
}

void Display::shutdown_noexcept() noexcept
{
    if (!module_open_) {
        return;
    }

    if (panel_ready_ && inkhibit_driver_error() == 0) {
        EPD_4IN0E_Sleep();
    }

    DEV_Module_Exit();

    module_open_ = false;
    panel_ready_ = false;
}

} // namespace inkhibit
