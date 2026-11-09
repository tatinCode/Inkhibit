#ifndef INKHIBIT_FAKE_HARDWARE_H
#define INKHIBIT_FAKE_HARDWARE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Unlike assert(), checks remain active in Release builds. */
#define CHECK(condition)                                              \
    do {                                                              \
        if (!(condition)) {                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #condition);                  \
            exit(EXIT_FAILURE);                                       \
        }                                                             \
    } while (0)

#define FRAME_SIZE 120000u
#define TRACE_CAPACITY (FRAME_SIZE + 256u)
#define NS_PER_MS UINT64_C(1000000)

struct spi_byte {
    uint8_t value;
    uint8_t dc;
    uint8_t cs;
};

struct fake_hardware {
    int chip_open;
    int spi_open;
    /* Adapter-owned claims: 0 = none, 1 = input, 2 = output.
     * GPIO8 belongs to the kernel SPI driver, not this claim table. */
    int claimed[32];
    int levels[32];

    unsigned chip_opens;
    unsigned chip_closes;
    unsigned spi_opens;
    unsigned spi_closes;
    unsigned frees;
    unsigned reads;
    unsigned writes;
    unsigned spi_calls;
    unsigned sleep_calls;
    unsigned clock_calls;

    /* Chip open, BUSY claim, three outputs, SPI open, power enable. */
    unsigned acquisition_step;
    unsigned fail_acquisition_at;
    int read_error;
    int write_error;
    unsigned fail_spi_at;
    int fail_spi_command_enabled;
    uint8_t fail_spi_command;
    int short_spi_write;
    int fail_free_pin;

    uint64_t now_ns;
    uint64_t ready_at_ns;
    unsigned busy_from_read;
    unsigned fail_clock_at;
    int sleep_error;
    int interrupt_sleep_once;

    struct spi_byte trace[TRACE_CAPACITY];
    size_t trace_size;
};

#ifdef __cplusplus
extern "C" {
#endif

extern struct fake_hardware fake;

int inkhibit_test_clock_gettime(clockid_t clock_id, struct timespec *now);
int inkhibit_test_nanosleep(
    const struct timespec *request,
    struct timespec *remaining
);

#ifdef __cplusplus
}
#endif

#endif
