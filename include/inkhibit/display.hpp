#ifndef INKHIBIT_DISPLAY_HPP
#define INKHIBIT_DISPLAY_HPP

#include "inkhibit/display_guard.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace inkhibit {

class Display {
public:
    static constexpr std::size_t native_width = 400;
    static constexpr std::size_t native_height = 600;
    static constexpr std::size_t framebuffer_size =
        native_width * native_height / 2;

    explicit Display(
        const std::filesystem::path& state_directory = "/var/lib/inkhibit"
    );

    ~Display() noexcept;

    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    Display(Display&&) = delete;
    Display& operator=(Display&&) = delete;

    // Initialize, transmit, refresh, sleep, and clean up.
    void show(const std::vector<std::uint8_t>& framebuffer);

    // Explicit shutdown, with error reporting.
    void close();

private:
    void shutdown_noexcept() noexcept;

    DisplayGuard guard_;
    bool attempted_ = false;
    bool module_open_ = false;
    bool panel_ready_ = false;
};

} // namespace inkhibit

#endif
