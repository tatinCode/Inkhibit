#include "fake_hardware.h"
#include "lgpio.h"

#include <errno.h>
#include <string.h>

/* Zero is a valid handle; verify the adapter accepts it. */
enum {
    CHIP_HANDLE = 0,
    SPI_HANDLE = 1,
    GPIO_BUSY = -79,
    INJECTED_ERROR = -99
};

/* Kernel-owned CE0 is inactive HIGH, including before lgSpiOpen(). */
struct fake_hardware fake = {
    .levels = {[8] = 1}
};

static int acquisition_fails(void)
{
    ++fake.acquisition_step;
    return fake.fail_acquisition_at != 0
        && fake.acquisition_step == fake.fail_acquisition_at;
}

static void check_gpio(int handle, int gpio)
{
    CHECK(handle == CHIP_HANDLE);
    CHECK(fake.chip_open);
    CHECK(gpio >= 0 && gpio < 32);
}

int lgGpiochipOpen(int gpio_dev)
{
    CHECK(gpio_dev == 0);
    CHECK(!fake.chip_open);
    ++fake.chip_opens;

    if (acquisition_fails()) {
        return INJECTED_ERROR;
    }

    fake.chip_open = 1;
    return CHIP_HANDLE;
}

int lgGpiochipClose(int handle)
{
    CHECK(handle == CHIP_HANDLE);
    CHECK(fake.chip_open);
    CHECK(!fake.spi_open);
    ++fake.chip_closes;
    fake.chip_open = 0;

    /* Chip closure releases even a line whose explicit free failed. */
    memset(fake.claimed, 0, sizeof(fake.claimed));
    return 0;
}

int lgGpioClaimInput(int handle, int flags, int gpio)
{
    check_gpio(handle, gpio);
    CHECK(flags == 0);
    CHECK(gpio == 24);
    CHECK(fake.claimed[gpio] == 0);

    if (acquisition_fails()) {
        return INJECTED_ERROR;
    }

    fake.claimed[gpio] = 1;
    return 0;
}

int lgGpioClaimOutput(int handle, int flags, int gpio, int level)
{
    check_gpio(handle, gpio);
    CHECK(flags == 0);

    /* Match the Pi: spi0 CS0 already owns this line. */
    if (gpio == 8) {
        return GPIO_BUSY;
    }

    CHECK(fake.claimed[gpio] == 0);
    CHECK(gpio == 17 || gpio == 25 || gpio == 18);
    CHECK(level == 0);

    if (acquisition_fails()) {
        return INJECTED_ERROR;
    }

    fake.claimed[gpio] = 2;
    fake.levels[gpio] = level;
    return 0;
}

int lgGpioFree(int handle, int gpio)
{
    check_gpio(handle, gpio);
    CHECK(gpio != 8);
    CHECK(fake.claimed[gpio] != 0);
    ++fake.frees;

    if (fake.fail_free_pin == gpio) {
        return INJECTED_ERROR;
    }

    fake.claimed[gpio] = 0;
    return 0;
}

int lgGpioRead(int handle, int gpio)
{
    check_gpio(handle, gpio);
    CHECK(fake.claimed[gpio] != 0);
    ++fake.reads;

    if (fake.read_error) {
        return INJECTED_ERROR;
    }

    if (gpio == 24) {
        if (fake.busy_from_read != 0
            && fake.reads >= fake.busy_from_read) {
            return 0;
        }

        return fake.now_ns >= fake.ready_at_ns ? 1 : 0;
    }

    return fake.levels[gpio];
}

int lgGpioWrite(int handle, int gpio, int level)
{
    check_gpio(handle, gpio);
    CHECK(gpio != 8);
    CHECK(fake.claimed[gpio] == 2);
    CHECK(level == 0 || level == 1);
    ++fake.writes;

    if (gpio == 18 && level == 1) {
        CHECK(fake.spi_open);
        if (acquisition_fails()) {
            return INJECTED_ERROR;
        }
    }

    if (fake.write_error) {
        return INJECTED_ERROR;
    }

    fake.levels[gpio] = level;
    return 0;
}

int lgSpiOpen(int device, int channel, int baud, int flags)
{
    CHECK(fake.chip_open);
    CHECK(!fake.spi_open);
    CHECK(device == 0);
    CHECK(channel == 0);
    CHECK(baud == 10000000);
    CHECK(flags == 0);
    CHECK(fake.claimed[24] == 1);
    CHECK(fake.claimed[17] == 2);
    CHECK(fake.claimed[25] == 2);
    CHECK(fake.claimed[8] == 0);
    CHECK(fake.levels[8] == 1);
    CHECK(fake.claimed[18] == 2);
    CHECK(fake.levels[18] == 0);
    ++fake.spi_opens;

    if (acquisition_fails()) {
        return INJECTED_ERROR;
    }

    fake.spi_open = 1;
    return SPI_HANDLE;
}

int lgSpiClose(int handle)
{
    CHECK(handle == SPI_HANDLE);
    CHECK(fake.spi_open);
    CHECK(fake.levels[8] == 1);
    ++fake.spi_closes;
    fake.spi_open = 0;
    return 0;
}

int lgSpiWrite(int handle, const char *data, int count)
{
    CHECK(handle == SPI_HANDLE);
    CHECK(fake.spi_open);
    CHECK(data != NULL);
    CHECK(count > 0);
    CHECK(fake.claimed[8] == 0);
    CHECK(fake.levels[8] == 1);
    ++fake.spi_calls;

    /* spidev mode 0 controls CE0 around each transfer, even on failure. */
    fake.levels[8] = 0;
    int result = count;
    if ((fake.fail_spi_at != 0 && fake.spi_calls == fake.fail_spi_at)
        || (fake.fail_spi_command_enabled && fake.levels[25] == 0
            && count == 1 && (uint8_t)data[0] == fake.fail_spi_command)) {
        result = INJECTED_ERROR;
    } else if (fake.short_spi_write) {
        result = count - 1;
    } else {
        CHECK((size_t)count <= TRACE_CAPACITY - fake.trace_size);
        for (int i = 0; i < count; ++i) {
            fake.trace[fake.trace_size++] = (struct spi_byte) {
                .value = (uint8_t)data[i],
                .dc = (uint8_t)fake.levels[25],
                .cs = (uint8_t)fake.levels[8]
            };
        }
    }

    fake.levels[8] = 1;
    return result;
}

const char *lguErrorText(int error)
{
    (void)error;
    return "injected fake hardware failure";
}

int inkhibit_test_clock_gettime(clockid_t clock_id, struct timespec *now)
{
    CHECK(clock_id == CLOCK_MONOTONIC);
    CHECK(now != NULL);
    ++fake.clock_calls;

    if (fake.fail_clock_at != 0 && fake.clock_calls == fake.fail_clock_at) {
        errno = EIO;
        return -1;
    }

    now->tv_sec = (time_t)(fake.now_ns / UINT64_C(1000000000));
    now->tv_nsec = (long)(fake.now_ns % UINT64_C(1000000000));
    return 0;
}

int inkhibit_test_nanosleep(
    const struct timespec *request,
    struct timespec *remaining)
{
    CHECK(request != NULL);
    CHECK(request->tv_sec >= 0);
    CHECK(request->tv_nsec >= 0 && request->tv_nsec < 1000000000L);
    ++fake.sleep_calls;

    if (fake.sleep_error) {
        errno = EINVAL;
        return -1;
    }

    /* The adapter aliases request and remaining: read before writing. */
    const uint64_t duration =
        (uint64_t)request->tv_sec * UINT64_C(1000000000)
        + (uint64_t)request->tv_nsec;

    if (fake.interrupt_sleep_once) {
        fake.interrupt_sleep_once = 0;
        const uint64_t elapsed = duration / 2;
        const uint64_t left = duration - elapsed;
        fake.now_ns += elapsed;

        if (remaining != NULL) {
            remaining->tv_sec = (time_t)(left / UINT64_C(1000000000));
            remaining->tv_nsec = (long)(left % UINT64_C(1000000000));
        }

        errno = EINTR;
        return -1;
    }

    fake.now_ns += duration;
    return 0;
}
