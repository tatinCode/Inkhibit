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
    lifecycle before_init kernel_cs cs_before_init faulted_reinit
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

foreach(stage RANGE 1 7)
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

# Compile the unmodified C++ sources with the normal system declarations.
set(display_check_sources
    "${PROJECT_SOURCE_DIR}/src/display.cpp"
    "${PROJECT_SOURCE_DIR}/src/display_guard.cpp"
)
if(EXISTS "${PROJECT_SOURCE_DIR}/src/display_test.cpp")
    list(APPEND display_check_sources "${PROJECT_SOURCE_DIR}/src/display_test.cpp")
else()
    message(STATUS "src/display_test.cpp is absent; checking wrapper/guard only")
endif()
add_library(inkhibit_display_compile_check OBJECT ${display_check_sources})

target_compile_features(
    inkhibit_display_compile_check PRIVATE cxx_std_17
)

set_target_properties(inkhibit_display_compile_check PROPERTIES
    CXX_EXTENSIONS OFF
)

target_compile_definitions(inkhibit_display_compile_check PRIVATE
    _POSIX_C_SOURCE=200809L
)

target_include_directories(inkhibit_display_compile_check PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${WAVESHARE_DIR}/Config"
    "${WAVESHARE_DIR}/e-Paper"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_display_compile_check PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

# Only this test copy of the guard gets a simulated boot clock. Target-local
# definitions keep the production library and compile check unaffected.
add_library(inkhibit_guard_under_test OBJECT
    "${PROJECT_SOURCE_DIR}/src/display_guard.cpp"
)
target_compile_definitions(inkhibit_guard_under_test PRIVATE
    clock_gettime=inkhibit_guard_test_clock_gettime
)

add_executable(inkhibit_display_tests
    display_tests.cpp
    fakes/fake_hardware.c
    "${PROJECT_SOURCE_DIR}/src/display.cpp"
)
target_link_libraries(inkhibit_display_tests PRIVATE
    inkhibit_guard_under_test
    inkhibit_driver_under_test
)

foreach(target IN ITEMS inkhibit_guard_under_test inkhibit_display_tests)
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES
        C_EXTENSIONS OFF
        CXX_EXTENSIONS OFF
    )
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/fakes"
        "${PROJECT_SOURCE_DIR}/include"
        "${WAVESHARE_DIR}/Config"
        "${WAVESHARE_DIR}/e-Paper"
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
target_compile_features(inkhibit_display_tests PRIVATE c_std_11)

set(display_cases
    lazy_init empty_frame short_frame long_frame invalid_colors validation_retry
    success one_shot_after_failure panel_init_timeout transmission_error
    refresh_timeout sleep_error cleanup_error state_write_error
    cooldown_before_hardware lock_before_hardware
)
foreach(case IN LISTS display_cases)
    add_test(NAME "display.${case}" COMMAND inkhibit_display_tests "display.${case}")
    set_tests_properties("display.${case}" PROPERTIES TIMEOUT 20)
endforeach()
foreach(stage RANGE 1 7)
    add_test(NAME "display.init_failure_${stage}"
        COMMAND inkhibit_display_tests display.init_failure "${stage}")
    set_tests_properties("display.init_failure_${stage}" PROPERTIES TIMEOUT 20)
endforeach()

set(guard_cases
    initial_uptime lifecycle begin_twice complete_idempotent rechecks_state
    lock_process cooldown_process interrupted_process invalid_state
    negative_timestamp boot_change clock_failure nonregular_state
    symlinks oversized_state
)
foreach(case IN LISTS guard_cases)
    add_test(NAME "guard.${case}" COMMAND inkhibit_display_tests "guard.${case}")
    set_tests_properties("guard.${case}" PROPERTIES TIMEOUT 20)
endforeach()
