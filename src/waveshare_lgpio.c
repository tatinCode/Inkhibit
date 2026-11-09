#include "DEV_Config.h"
#include "inkhibit/driver_status.h"
#include "pi_config.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lgpio.h>

static int gpio_handle = -1;
static int spi_handle = -1;
static int module_ready = 0;

static int first_error = 0;
static char first_error_message[256];

/* PWR starts LOW until all resources have been acquired. */
static const int output_pins[] = {17, 25, 8, 18};
static const int initial_levels[] = {0, 0, 1, 0};

static unsigned int claimed_outputs = 0;
static int busy_claimed = 0;

#define OUTPUT_COUNT \
    (sizeof(output_pins) / sizeof(output_pins[0]))

int inkhibit_driver_error(void)
{
    return first_error;
}

const char *inkhibit_driver_error_message(void)
{
    return first_error_message;
}

void inkhibit_driver_fail(int code, const char *message)
{
    if (first_error != 0 || code == 0) {
        return;
    }

    first_error = code;

    snprintf(
        first_error_message,
        sizeof(first_error_message),
        "%s",
        message != NULL ? message : "Unspecified driver error"
    );
}

static int check_lgpio(int result, const char *operation)
{
    if (result >= 0) {
        return 0;
    }

    char message[256];

    snprintf(
        message,
        sizeof(message),
        "%s: %s (lgpio error %d)",
        operation,
        lguErrorText(result),
        result
    );

    inkhibit_driver_fail(EIO, message);
    return -1;
}

static int require_ready(void)
{
    if (first_error != 0) {
        return -1;
    }

    if (!module_ready) {
        inkhibit_driver_fail(
            ENODEV,
            "Display GPIO/SPI resources are not initialized"
        );
        return -1;
    }

    return 0;
}

void DEV_Module_Exit(void)
{
    /*
     * Use lgpio directly here: cleanup must still be attempted when an
     * earlier operation has latched an error.
     *
     * The caller should send the panel's sleep command before normal
     * cleanup. On failure, lowering PWR is best-effort shutdown.
     */
    module_ready = 0;

    if (spi_handle >= 0) {
        check_lgpio(lgSpiClose(spi_handle), "Closing SPI");
        spi_handle = -1;
    }

    if (gpio_handle >= 0) {
        /* Lower PWR before changing the other output levels. */
        if ((claimed_outputs & (1u << 3)) != 0) {
            check_lgpio(
                lgGpioWrite(gpio_handle, EPD_PWR_PIN, 0),
                "Lowering display PWR"
            );
        }

        for (size_t i = 0; i < OUTPUT_COUNT; ++i) {
            if ((claimed_outputs & (1u << i)) == 0) {
                continue;
            }

            /* Leave CS inactive; lower reset, DC and power. */
            const int level = output_pins[i] == EPD_CS_PIN ? 1 : 0;

            check_lgpio(
                lgGpioWrite(gpio_handle, output_pins[i], level),
                "Setting GPIO shutdown level"
            );

            check_lgpio(
                lgGpioFree(gpio_handle, output_pins[i]),
                "Releasing output GPIO"
            );
        }

        if (busy_claimed) {
            check_lgpio(
                lgGpioFree(gpio_handle, EPD_BUSY_PIN),
                "Releasing BUSY GPIO"
            );
        }

        check_lgpio(
            lgGpiochipClose(gpio_handle),
            "Closing GPIO chip"
        );

        gpio_handle = -1;
    }

    claimed_outputs = 0;
    busy_claimed = 0;
}

UBYTE DEV_Module_Init(void)
{
    /*
     * Repeating a successful initialization does not reopen handles.
     * After an error in an open session, require cleanup before retrying.
     */
    if (module_ready) {
        return first_error == 0 ? 0 : 1;
    }

    first_error = 0;
    first_error_message[0] = '\0';

    /* Pi Zero 2 W: /dev/gpiochip0. */
    gpio_handle = lgGpiochipOpen(0);

    if (check_lgpio(gpio_handle, "Opening gpiochip0") != 0) {
        goto fail;
    }

    if (check_lgpio(
            lgGpioClaimInput(gpio_handle, 0, EPD_BUSY_PIN),
            "Claiming BUSY GPIO"
        ) != 0) {
        goto fail;
    }

    busy_claimed = 1;

    for (size_t i = 0; i < OUTPUT_COUNT; ++i) {
        if (check_lgpio(
                lgGpioClaimOutput(
                    gpio_handle,
                    0,
                    output_pins[i],
                    initial_levels[i]
                ),
                "Claiming output GPIO"
            ) != 0) {
            goto fail;
        }

        claimed_outputs |= 1u << i;
    }

    /*
     * Same SPI settings as the official lgpio backend:
     * bus 0, device 0, 10 MHz, mode 0.
     */
    spi_handle = lgSpiOpen(0, 0, 10000000, 0);

    if (check_lgpio(spi_handle, "Opening spidev0.0") != 0) {
        goto fail;
    }

    if (check_lgpio(
            lgGpioWrite(gpio_handle, EPD_PWR_PIN, 1),
            "Enabling display PWR"
        ) != 0) {
        goto fail;
    }

    module_ready = 1;
    return 0;

fail:
    DEV_Module_Exit();
    return 1;
}

void DEV_Digital_Write(UWORD pin, UBYTE value)
{
    if (require_ready() != 0) {
        return;
    }

    check_lgpio(
        lgGpioWrite(gpio_handle, pin, value),
        "Writing GPIO"
    );
}

UBYTE DEV_Digital_Read(UWORD pin)
{
    if (require_ready() != 0) {
        return 0;
    }

    const int result = lgGpioRead(gpio_handle, pin);

    /*
     * Check the signed result before conversion to UBYTE.
     * A negative lgpio error must never look like a HIGH input.
     */
    if (check_lgpio(result, "Reading GPIO") != 0) {
        return 0;
    }

    return (UBYTE)result;
}

void DEV_SPI_Write_nByte(uint8_t *data, uint32_t length)
{
    if (require_ready() != 0) {
        return;
    }

    if (length == 0) {
        return;
    }

    if (data == NULL || length > (uint32_t)INT_MAX) {
        inkhibit_driver_fail(EINVAL, "Invalid SPI buffer or length");
        return;
    }

    const int written = lgSpiWrite(
        spi_handle,
        (const char *)data,
        (int)length
    );

    if (check_lgpio(written, "Writing SPI") != 0) {
        return;
    }

    /*
     * lgSpiWrite returns the byte count, not merely zero on success.
     * Do not retry a partial transfer: the panel's state is uncertain.
     */
    if (written != (int)length) {
        inkhibit_driver_fail(EIO, "Incomplete SPI write");
    }
}

void DEV_SPI_WriteByte(uint8_t value)
{
    DEV_SPI_Write_nByte(&value, 1);
}

void DEV_Delay_ms(UDOUBLE milliseconds)
{
    if (first_error != 0) {
        return;
    }

    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };

    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            inkhibit_driver_fail(errno, "nanosleep failed");
            return;
        }
    }
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        inkhibit_driver_fail(errno, "Reading monotonic clock failed");
        return 0;
    }

    return (uint64_t)now.tv_sec * UINT64_C(1000)
        + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

int inkhibit_driver_wait_ready(uint32_t timeout_ms)
{
    if (require_ready() != 0) {
        return -1;
    }

    const uint64_t started = monotonic_ms();

    if (first_error != 0) {
        return -1;
    }

    for (;;) {
        const UBYTE level = DEV_Digital_Read(EPD_BUSY_PIN);

        if (first_error != 0) {
            return -1;
        }

        /* Official driver semantics: LOW = busy, HIGH = idle. */
        if (level != 0) {
            DEV_Delay_ms(100);
            return first_error == 0 ? 0 : -1;
        }

        const uint64_t now = monotonic_ms();

        if (first_error != 0) {
            return -1;
        }

        if (now - started >= timeout_ms) {
            inkhibit_driver_fail(
                ETIMEDOUT,
                "Timed out waiting for display BUSY to become HIGH"
            );
            return -1;
        }

        DEV_Delay_ms(10);

        if (first_error != 0) {
            return -1;
        }
    }
}
