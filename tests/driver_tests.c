#include "DEV_Config.h"
#include "EPD_4in0e.h"
#include "inkhibit/driver_status.h"
#include "fake_hardware.h"

#include <errno.h>
#include <string.h>

static uint8_t frame[FRAME_SIZE];

static void start_module(void)
{
    CHECK(DEV_Module_Init() == 0);
    CHECK(inkhibit_driver_error() == 0);
    CHECK(fake.chip_open);
    CHECK(fake.spi_open);
    CHECK(fake.levels[18] == 1);
}

static void check_closed(void)
{
    CHECK(!fake.chip_open);
    CHECK(!fake.spi_open);
    for (size_t i = 0; i < 32; ++i) {
        CHECK(fake.claimed[i] == 0);
    }
}

static void start_panel(void)
{
    start_module();
    EPD_4IN0E_Init();
    CHECK(inkhibit_driver_error() == 0);
    fake.trace_size = 0;
    fake.spi_calls = 0;
    fake.reads = 0;
}

static void expect_commands(const uint8_t *expected, size_t count)
{
    size_t found = 0;
    for (size_t i = 0; i < fake.trace_size; ++i) {
        CHECK(fake.trace[i].cs == 0);
        if (fake.trace[i].dc == 0) {
            CHECK(found < count);
            CHECK(fake.trace[i].value == expected[found]);
            ++found;
        }
    }
    CHECK(found == count);
}

static void test_lifecycle(void)
{
    start_module();
    CHECK(DEV_Module_Init() == 0);
    CHECK(fake.chip_opens == 1);
    CHECK(fake.spi_opens == 1);

    DEV_Module_Exit();
    check_closed();
    CHECK(fake.levels[18] == 0);
    CHECK(fake.frees == 5);

    const unsigned frees = fake.frees;
    const unsigned closes = fake.chip_closes;
    DEV_Module_Exit();
    CHECK(fake.frees == frees);
    CHECK(fake.chip_closes == closes);

    start_module();
    CHECK(fake.chip_opens == 2);
    CHECK(fake.spi_opens == 2);
}

static void test_init_failure(unsigned stage)
{
    CHECK(stage >= 1 && stage <= 8);
    fake.fail_acquisition_at = stage;
    CHECK(DEV_Module_Init() != 0);
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.acquisition_step == stage);
    check_closed();

    /* Failed initialization must leave a new session possible. */
    fake.fail_acquisition_at = 0;
    start_module();
}

static void test_before_init(void)
{
    CHECK(DEV_Digital_Read(24) == 0);
    CHECK(inkhibit_driver_error() == ENODEV);
    CHECK(fake.reads == 0);
    CHECK(fake.chip_opens == 0);
    start_module();
}

static void test_faulted_reinit(void)
{
    start_module();
    inkhibit_driver_fail(EIO, "session failure");
    CHECK(DEV_Module_Init() != 0);
    CHECK(fake.chip_opens == 1);
    CHECK(fake.spi_opens == 1);
    CHECK(inkhibit_driver_error() == EIO);

    DEV_Module_Exit();
    start_module();
    CHECK(fake.chip_opens == 2);
}

static void test_gpio_read_error(void)
{
    start_module();
    fake.read_error = 1;
    CHECK(DEV_Digital_Read(24) == 0);
    CHECK(inkhibit_driver_error() == EIO);

    const unsigned writes = fake.writes;
    const unsigned reads = fake.reads;
    DEV_Digital_Write(25, 1);
    DEV_SPI_WriteByte(0xAA);
    (void)DEV_Digital_Read(24);
    CHECK(fake.writes == writes);
    CHECK(fake.reads == reads);
    CHECK(fake.spi_calls == 0);
}

static void test_gpio_write_error(void)
{
    start_module();
    fake.write_error = 1;
    DEV_Digital_Write(25, 1);
    CHECK(inkhibit_driver_error() == EIO);
    DEV_SPI_WriteByte(0xAA);
    CHECK(fake.spi_calls == 0);
}

static void test_spi_error(void)
{
    start_module();
    fake.fail_spi_at = 1;
    DEV_SPI_WriteByte(0xAA);
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.spi_calls == 1);
    DEV_SPI_WriteByte(0xBB);
    CHECK(fake.spi_calls == 1);
}

static void test_spi_short(void)
{
    start_module();
    fake.short_spi_write = 1;
    uint8_t bytes[] = {0x12, 0x35, 0x60};
    DEV_SPI_Write_nByte(bytes, sizeof(bytes));
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.spi_calls == 1);
    CHECK(strstr(inkhibit_driver_error_message(), "Incomplete SPI write") != NULL);
}

static void test_spi_null(void)
{
    start_module();
    DEV_SPI_Write_nByte(NULL, 0);
    CHECK(inkhibit_driver_error() == 0);
    CHECK(fake.spi_calls == 0);
    DEV_SPI_Write_nByte(NULL, 1);
    CHECK(inkhibit_driver_error() == EINVAL);
    CHECK(fake.spi_calls == 0);
}

static void test_spi_oversized(void)
{
    start_module();
    uint8_t byte = 0;
    DEV_SPI_Write_nByte(&byte, UINT32_MAX);
    CHECK(inkhibit_driver_error() == EINVAL);
    CHECK(fake.spi_calls == 0);
}

static void test_busy_immediate(void)
{
    start_module();
    CHECK(inkhibit_driver_wait_ready(1000) == 0);
    CHECK(fake.reads == 1);
    CHECK(fake.now_ns == 100 * NS_PER_MS);
}

static void test_busy_ready(void)
{
    start_module();
    fake.ready_at_ns = 30 * NS_PER_MS;
    CHECK(inkhibit_driver_wait_ready(1000) == 0);
    CHECK(inkhibit_driver_error() == 0);
    /* Readiness at 30 ms, followed by the 100 ms settling delay. */
    CHECK(fake.now_ns == 130 * NS_PER_MS);
}

static void test_busy_timeout(void)
{
    start_module();
    fake.ready_at_ns = UINT64_MAX;
    CHECK(inkhibit_driver_wait_ready(25) == -1);
    CHECK(inkhibit_driver_error() == ETIMEDOUT);
    CHECK(fake.now_ns >= 25 * NS_PER_MS);
    CHECK(fake.now_ns <= 35 * NS_PER_MS);
}

static void test_busy_zero_timeout(void)
{
    start_module();
    fake.ready_at_ns = UINT64_MAX;
    CHECK(inkhibit_driver_wait_ready(0) == -1);
    CHECK(inkhibit_driver_error() == ETIMEDOUT);
    CHECK(fake.sleep_calls == 0);
    CHECK(fake.now_ns == 0);
}

static void test_busy_read_error(void)
{
    start_module();
    fake.read_error = 1;
    CHECK(inkhibit_driver_wait_ready(1000) == -1);
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.sleep_calls == 0);
}

static void test_clock_error(void)
{
    start_module();
    fake.fail_clock_at = 1;
    CHECK(inkhibit_driver_wait_ready(1000) == -1);
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.reads == 0);
}

static void test_clock_poll_error(void)
{
    start_module();
    fake.ready_at_ns = UINT64_MAX;
    fake.fail_clock_at = 2;
    CHECK(inkhibit_driver_wait_ready(1000) == -1);
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.reads == 1);
    CHECK(fake.sleep_calls == 0);
}

static void test_interrupted_sleep(void)
{
    start_module();
    fake.interrupt_sleep_once = 1;
    DEV_Delay_ms(100);
    CHECK(inkhibit_driver_error() == 0);
    CHECK(fake.sleep_calls == 2);
    CHECK(fake.now_ns == 100 * NS_PER_MS);
}

static void test_sleep_error(void)
{
    start_module();
    fake.sleep_error = 1;
    DEV_Delay_ms(10);
    CHECK(inkhibit_driver_error() == EINVAL);
}

static void test_error_preserved(void)
{
    start_module();
    char message[] = "original failure";
    inkhibit_driver_fail(ETIMEDOUT, message);
    message[0] = 'X';
    inkhibit_driver_fail(EINVAL, "second failure");
    fake.fail_free_pin = 17;
    DEV_Module_Exit();
    CHECK(inkhibit_driver_error() == ETIMEDOUT);
    CHECK(strcmp(inkhibit_driver_error_message(), "original failure") == 0);
    check_closed();

    fake.fail_free_pin = 0;
    start_module();
    CHECK(inkhibit_driver_error_message()[0] == '\0');
}

static void test_cleanup_error(void)
{
    start_module();
    fake.fail_free_pin = 17;
    DEV_Module_Exit();
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.frees == 5);
    CHECK(fake.spi_closes == 1);
    CHECK(fake.chip_closes == 1);
    check_closed();
}

static void test_panel_init(void)
{
    start_module();
    EPD_4IN0E_Init();
    CHECK(inkhibit_driver_error() == 0);
    static const uint8_t commands[] = {
        0xAA, 0x01, 0x00, 0x05, 0x08, 0x06, 0x03,
        0x60, 0x30, 0x50, 0x61, 0xE3, 0x84
    };
    expect_commands(commands, sizeof(commands));

    /* Independent contract: the controller is programmed for 400 x 600. */
    static const uint8_t resolution[] = {0x01, 0x90, 0x02, 0x58};
    int found_resolution = 0;
    for (size_t i = 0; i + 4 < fake.trace_size; ++i) {
        if (fake.trace[i].dc == 0 && fake.trace[i].value == 0x61) {
            for (size_t j = 0; j < sizeof(resolution); ++j) {
                CHECK(fake.trace[i + 1 + j].dc == 1);
                CHECK(fake.trace[i + 1 + j].value == resolution[j]);
            }
            found_resolution = 1;
        }
    }
    CHECK(found_resolution);
}

static void test_panel_init_timeout(void)
{
    start_module();
    fake.ready_at_ns = UINT64_MAX;
    EPD_4IN0E_Init();
    CHECK(inkhibit_driver_error() == ETIMEDOUT);
    CHECK(fake.trace_size == 0);
    CHECK(fake.now_ns >= UINT64_C(60000) * NS_PER_MS);
}

static void test_framebuffer(void)
{
    start_panel();
    static const uint8_t colors[] = {0, 1, 2, 3, 5, 6};
    for (size_t i = 0; i < FRAME_SIZE; ++i) {
        frame[i] = (uint8_t)((colors[i % 6] << 4) | colors[(i / 6) % 6]);
    }

    EPD_4IN0E_Display(frame);
    CHECK(inkhibit_driver_error() == 0);
    CHECK(fake.trace_size == FRAME_SIZE + 11);
    CHECK(fake.trace[0].dc == 0);
    CHECK(fake.trace[0].value == 0x10);
    for (size_t i = 0; i < FRAME_SIZE; ++i) {
        CHECK(fake.trace[i + 1].dc == 1);
        CHECK(fake.trace[i + 1].value == frame[i]);
    }

    static const uint8_t commands[] = {0x10, 0x04, 0x06, 0x12, 0x02};
    expect_commands(commands, sizeof(commands));
    static const uint8_t tail[] = {
        0x04, 0x06, 0x6F, 0x1F, 0x17, 0x27, 0x12, 0x00, 0x02, 0x00
    };
    static const uint8_t tail_dc[] = {0, 0, 1, 1, 1, 1, 0, 1, 0, 1};
    for (size_t i = 0; i < sizeof(tail); ++i) {
        CHECK(fake.trace[FRAME_SIZE + 1 + i].value == tail[i]);
        CHECK(fake.trace[FRAME_SIZE + 1 + i].dc == tail_dc[i]);
    }
}

static void test_transmission_error(void)
{
    start_panel();
    fake.fail_spi_at = 17;
    EPD_4IN0E_Display(frame);
    CHECK(inkhibit_driver_error() == EIO);
    CHECK(fake.spi_calls == 17);
    static const uint8_t commands[] = {0x10};
    expect_commands(commands, sizeof(commands));
    EPD_4IN0E_Sleep();
    CHECK(fake.spi_calls == 17);
}

static void test_refresh_timeout(unsigned stage)
{
    CHECK(stage >= 1 && stage <= 3);
    start_panel();
    /* Fail the POWER_ON, DISPLAY_REFRESH, or POWER_OFF BUSY wait. */
    fake.busy_from_read = stage;
    EPD_4IN0E_Display(frame);
    CHECK(inkhibit_driver_error() == ETIMEDOUT);
    static const uint8_t commands[] = {0x10, 0x04, 0x06, 0x12, 0x02};
    const size_t count = stage == 1 ? 2 : (stage == 2 ? 4 : 5);
    expect_commands(commands, count);
    const size_t transmitted = fake.trace_size;
    EPD_4IN0E_Sleep();
    CHECK(fake.trace_size == transmitted);
}

static void test_null_frame(void)
{
    start_module();
    EPD_4IN0E_Display(NULL);
    CHECK(inkhibit_driver_error() == EINVAL);
    CHECK(fake.trace_size == 0);
}

static void test_panel_sleep(void)
{
    start_panel();
    const uint64_t before = fake.now_ns;
    EPD_4IN0E_Sleep();
    CHECK(inkhibit_driver_error() == 0);
    CHECK(fake.trace_size == 2);
    CHECK(fake.trace[0].dc == 0);
    CHECK(fake.trace[0].cs == 0);
    CHECK(fake.trace[0].value == 0x07);
    CHECK(fake.trace[1].dc == 1);
    CHECK(fake.trace[1].cs == 0);
    CHECK(fake.trace[1].value == 0xA5);
    CHECK(fake.now_ns - before == 2000 * NS_PER_MS);
}

struct test_case {
    const char *name;
    void (*run)(void);
};

int main(int argc, char **argv)
{
    CHECK(argc == 2 || argc == 3);
    /* Also require every vendor-compatible pin symbol to link. */
    CHECK(EPD_RST_PIN == 17);
    CHECK(EPD_DC_PIN == 25);
    CHECK(EPD_CS_PIN == 8);
    CHECK(EPD_BUSY_PIN == 24);
    CHECK(EPD_PWR_PIN == 18);
    CHECK(EPD_MOSI_PIN == 10);
    CHECK(EPD_SCLK_PIN == 11);

    static const struct test_case tests[] = {
        {"lifecycle", test_lifecycle},
        {"before_init", test_before_init},
        {"faulted_reinit", test_faulted_reinit},
        {"gpio_read_error", test_gpio_read_error},
        {"gpio_write_error", test_gpio_write_error},
        {"spi_error", test_spi_error},
        {"spi_short", test_spi_short},
        {"spi_null", test_spi_null},
        {"spi_oversized", test_spi_oversized},
        {"busy_immediate", test_busy_immediate},
        {"busy_ready", test_busy_ready},
        {"busy_timeout", test_busy_timeout},
        {"busy_zero_timeout", test_busy_zero_timeout},
        {"busy_read_error", test_busy_read_error},
        {"clock_error", test_clock_error},
        {"clock_poll_error", test_clock_poll_error},
        {"interrupted_sleep", test_interrupted_sleep},
        {"sleep_error", test_sleep_error},
        {"error_preserved", test_error_preserved},
        {"cleanup_error", test_cleanup_error},
        {"panel_init", test_panel_init},
        {"panel_init_timeout", test_panel_init_timeout},
        {"framebuffer", test_framebuffer},
        {"transmission_error", test_transmission_error},
        {"null_frame", test_null_frame},
        {"panel_sleep", test_panel_sleep}
    };

    int found = 0;
    if (strcmp(argv[1], "init_failure") == 0) {
        CHECK(argc == 3);
        test_init_failure((unsigned)atoi(argv[2]));
        found = 1;
    } else if (strcmp(argv[1], "refresh_timeout") == 0) {
        CHECK(argc == 3);
        test_refresh_timeout((unsigned)atoi(argv[2]));
        found = 1;
    } else {
        CHECK(argc == 2);
        for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
            if (strcmp(argv[1], tests[i].name) == 0) {
                tests[i].run();
                found = 1;
                break;
            }
        }
    }

    CHECK(found);
    DEV_Module_Exit();
    check_closed();
    printf("PASS: %s\n", argv[1]);
    return EXIT_SUCCESS;
}
