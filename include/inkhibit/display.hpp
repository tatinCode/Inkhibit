#ifndef INKHIBIT_DISPLAY_HPP
#define INKHIBIT_DISPLAY_HPP

#include "inkhibit/display_guard.hpp"
#include "inkhibit/framebuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace inkhibit {

class Display {
public:
    static constexpr std::size_t native_width = 
        framebuffer::native_width;

    static constexpr std::size_t native_height = 
        framebuffer::native_height;

    static constexpr std::size_t framebuffer_size =
        framebuffer::framebuffer_size;

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
