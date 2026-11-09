#ifndef INKHIBIT_DISPLAY_GUARD
#define INKHIBIT_DISPLAY_GUARD

#include <cstdint>
#include <filesystem>
#include <string>

namespace inkhibit {

class DisplayGuard {
public:
    static constexpr std::uint64_t cooldown_ms = 180'000;

    explicit DisplayGuard(
        const std::filesystem::path& state_directory = "/var/lib/inkhibit"
    );

    ~DisplayGuard() noexcept;

    DisplayGuard(const DisplayGuard&) = delete;
    DisplayGuard& operator=(const DisplayGuard&) = delete;
    DisplayGuard(DisplayGuard&&) = delete;
    DisplayGuard& operator=(DisplayGuard&&) = delete;

    // Persist an in-progress marker before touching hardware.
    void begin();

    // Record a cooldown after successful hardware shutdown.
    void complete();

private:
    void enforce_cooldown();

    void write_state(
        const char* phase,
        std::uint64_t not_before_ms
    );

    int directory_fd_ = -1;
    int lock_fd_ = -1;

    std::string boot_id_;
    bool active_ = false;
};

} // namespace inkhibit

#endif
