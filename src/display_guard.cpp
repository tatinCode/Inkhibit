#include "inkhibit/display_guard.hpp"

#include <cerrno>
#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <charconv>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int value) noexcept
        : value_(value) {
    }

    ~FileDescriptor() noexcept {
        if (value_ >= 0) {
            ::close(value_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept {
        return value_;
    }

private:
    int value_;
};

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(
        errno,
        std::generic_category(),
        operation
    );
}

std::uint64_t boottime_ms() {
    timespec now{};

    if (::clock_gettime(CLOCK_BOOTTIME, &now) != 0) {
        throw_errno("Reading boot clock");
    }

    return static_cast<std::uint64_t>(now.tv_sec) * 1000
        + static_cast<std::uint64_t>(now.tv_nsec) / 1'000'000;
}

std::string read_boot_id() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string id;

    if (!std::getline(input, id) || id.empty()) {
        throw std::runtime_error("Cannot read the Linux boot ID");
    }

    return id;
}

void require_regular_file(int fd) {
    struct stat info{};

    if (::fstat(fd, &info) != 0) {
        throw_errno("Inspecting display state file");
    }

    if (!S_ISREG(info.st_mode)) {
        throw std::runtime_error(
            "Display lock/state path must be a regular file"
        );
    }
}

std::optional<std::string> read_state(int directory_fd) {
    FileDescriptor file(::openat(
        directory_fd,
        "display.state",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
    ));

    if (file.get() < 0) {
        if (errno == ENOENT) {
            return std::nullopt;
        }

        throw_errno("Opening display state");
    }

    require_regular_file(file.get());

    std::string contents;
    char buffer[128];

    for (;;) {
        const ssize_t count = ::read(
            file.get(),
            buffer,
            sizeof(buffer)
        );

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw_errno("Reading display state");
        }

        if (count == 0) {
            break;
        }

        contents.append(buffer, static_cast<std::size_t>(count));

        if (contents.size() > 256) {
            throw std::runtime_error(
                "Display state is unexpectedly large; hardware was not initialized"
            );
        }
    }

    return contents;
}

void write_all(int fd, const std::string& contents) {
    std::size_t offset = 0;

    while (offset < contents.size()) {
        const ssize_t count = ::write(
            fd,
            contents.data() + offset,
            contents.size() - offset
        );

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw_errno("Writing display state");
        }

        if (count == 0) {
            throw std::runtime_error("Display state write made no progress");
        }

        offset += static_cast<std::size_t>(count);
    }
}

[[noreturn]] void throw_cooldown(std::uint64_t remaining_ms) {
    const std::uint64_t seconds =
        remaining_ms / 1000 + (remaining_ms % 1000 != 0);

    throw std::runtime_error(
        "Display cooldown active; try again in "
        + std::to_string(seconds)
        + " seconds"
    );
}

} // namespace

namespace inkhibit {

DisplayGuard::DisplayGuard( const std::filesystem::path& state_directory) {
    try {
        std::filesystem::create_directories(state_directory);

        directory_fd_ = ::open(
            state_directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        );

        if (directory_fd_ < 0) {
            throw_errno("Opening display state directory");
        }

        lock_fd_ = ::openat(
            directory_fd_,
            "display.lock",
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
            0600
        );

        if (lock_fd_ < 0) {
            throw_errno("Opening display lock");
        }

        require_regular_file(lock_fd_);

        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                throw std::runtime_error(
                    "Another Inkhibit process owns the display"
                );
            }

            throw_errno("Locking display");
        }

        boot_id_ = read_boot_id();
        enforce_cooldown();
    } catch (...) {
        if (lock_fd_ >= 0) {
            ::close(lock_fd_);
        }

        if (directory_fd_ >= 0) {
            ::close(directory_fd_);
        }

        throw;
    }
}

DisplayGuard::~DisplayGuard() noexcept {
    // Closing the lock descriptor releases flock.
    if (lock_fd_ >= 0) {
        ::close(lock_fd_);
    }

    if (directory_fd_ >= 0) {
        ::close(directory_fd_);
    }

    // An unfinished session intentionally retains its active state marker.
}

void DisplayGuard::write_state(
    const char* phase,
    std::uint64_t not_before_ms) {
    const std::string contents =
        "INKHIBIT_DISPLAY_V1 "
        + boot_id_ + " "
        + phase + " "
        + std::to_string(not_before_ms)
        + "\n";

    FileDescriptor temporary(::openat(
        directory_fd_,
        "display.state.tmp",
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        0600
    ));

    if (temporary.get() < 0) {
        throw_errno("Opening temporary display state");
    }

    require_regular_file(temporary.get());
    write_all(temporary.get(), contents);

    if (::fsync(temporary.get()) != 0) {
        throw_errno("Synchronizing display state");
    }

    if (::renameat(
            directory_fd_,
            "display.state.tmp",
            directory_fd_,
            "display.state"
        ) != 0) {
        throw_errno("Replacing display state");
    }

    if (::fsync(directory_fd_) != 0) {
        throw_errno("Synchronizing display state directory");
    }
}

void DisplayGuard::enforce_cooldown() {
    const auto contents = read_state(directory_fd_);
    const std::uint64_t now = boottime_ms();

    if (!contents) {
        /*
         * For a new state directory, wait until the Pi has been booted
         * for at least 180 seconds before the first update.
         */
        if (now < cooldown_ms) {
            throw_cooldown(cooldown_ms - now);
        }

        return;
    }

    std::istringstream input(*contents);

    std::string version;
    std::string previous_boot;
    std::string phase;
    std::string timestamp_text;
    std::string extra;
    std::uint64_t not_before = 0;

    bool valid =
        static_cast<bool>(
            input >> version >> previous_boot >> phase >> timestamp_text
            )
        && !(input >> extra)
        && version == "INKHIBIT_DISPLAY_V1"
        && (phase == "idle" || phase == "active");

    if(valid){
        const char* first = timestamp_text.data();
        const char* last = first + timestamp_text.size();

        const auto result = std::from_chars(first, last, not_before);

        valid = result.ec == std::errc{}
            && result.ptr == last;
    }

    if (!valid || previous_boot != boot_id_ || phase == "active") {
        /*
         * Persist recovery once. Subsequent attempts read an ordinary
         * cooldown rather than restarting the 180-second countdown.
         */
        write_state("idle", now + cooldown_ms);

        throw std::runtime_error(
            "Previous display state was interrupted, invalid, or from another "
            "boot; a 180-second recovery cooldown has been recorded"
        );
    }

    if (now < not_before) {
        throw_cooldown(not_before - now);
    }
}

void DisplayGuard::begin() {
    if (active_) {
        throw std::logic_error("Display session already started");
    }

    enforce_cooldown();

    // Do not touch hardware unless this marker has been persisted.
    write_state("active", 0);
    active_ = true;
}

void DisplayGuard::complete() {
    if (!active_) {
        return;
    }

    write_state("idle", boottime_ms() + cooldown_ms);
    active_ = false;
}

} // namespace inkhibit
