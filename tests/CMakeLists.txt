add_library(inkhibit_driver_under_test OBJECT
    "${PROJECT_SOURCE_DIR}/src/waveshare_lgpio.c"
    "${WAVESHARE_DIR}/e-Paper/EPD_4in0e.c"
)

add_executable(inkhibit_driver_tests
    driver_tests.c
    fakes/fake_hardware.c
)

target_link_libraries(inkhibit_driver_tests PRIVATE
    inkhibit_driver_under_test
)

foreach(target IN ITEMS inkhibit_driver_under_test inkhibit_driver_tests)
    target_compile_features(${target} PRIVATE c_std_11)
    set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)

    target_compile_definitions(${target} PRIVATE
        _POSIX_C_SOURCE=200809L
        RPI=1
        USE_LGPIO_LIB=1
        INKHIBIT_CHECKED_DRIVER=1
    )

    # The fake header must precede any system lgpio header.
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/fakes"
        "${PROJECT_SOURCE_DIR}/include"
        "${WAVESHARE_DIR}/Config"
        "${WAVESHARE_DIR}/e-Paper"
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Werror=implicit-function-declaration
        )
    endif()
endforeach()

# Remap time only in the test copy of the actual driver sources.
# Production sources and the fake implementation use their normal declarations.
target_compile_definitions(inkhibit_driver_under_test PRIVATE
    clock_gettime=inkhibit_test_clock_gettime
    nanosleep=inkhibit_test_nanosleep
)

set(driver_cases
    lifecycle before_init faulted_reinit
    gpio_read_error gpio_write_error
    spi_error spi_short spi_null spi_oversized
    busy_immediate busy_ready busy_timeout busy_zero_timeout busy_read_error
    clock_error clock_poll_error interrupted_sleep sleep_error
    error_preserved cleanup_error panel_init panel_init_timeout
    framebuffer transmission_error null_frame panel_sleep
)

foreach(case IN LISTS driver_cases)
    add_test(NAME "driver.${case}" COMMAND inkhibit_driver_tests "${case}")
    set_tests_properties("driver.${case}" PROPERTIES TIMEOUT 10)
endforeach()

foreach(stage RANGE 1 8)
    add_test(
        NAME "driver.init_failure_${stage}"
        COMMAND inkhibit_driver_tests init_failure "${stage}"
    )
    set_tests_properties("driver.init_failure_${stage}" PROPERTIES TIMEOUT 10)
endforeach()

foreach(stage RANGE 1 3)
    add_test(
        NAME "driver.refresh_timeout_${stage}"
        COMMAND inkhibit_driver_tests refresh_timeout "${stage}"
    )
    set_tests_properties("driver.refresh_timeout_${stage}" PROPERTIES TIMEOUT 10)
endforeach()

# Compile the unchecked vendor branch without linking it or accessing hardware.
add_library(inkhibit_vendor_compile_check OBJECT
    "${WAVESHARE_DIR}/e-Paper/EPD_4in0e.c"
)
target_compile_features(inkhibit_vendor_compile_check PRIVATE c_std_11)
set_target_properties(inkhibit_vendor_compile_check PROPERTIES C_EXTENSIONS OFF)
target_include_directories(inkhibit_vendor_compile_check PRIVATE
    "${WAVESHARE_DIR}/Config"
    "${WAVESHARE_DIR}/e-Paper"
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_vendor_compile_check PRIVATE
        -Wall -Wextra -Werror=implicit-function-declaration
    )
endif()
