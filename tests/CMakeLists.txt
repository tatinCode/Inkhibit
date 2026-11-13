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
    "${PROJECT_SOURCE_DIR}/src/display_image.cpp"
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

# Pure framebuffer tests: no driver, fake hardware, or Linux state files.
add_executable(inkhibit_framebuffer_tests framebuffer_tests.cpp)
target_link_libraries(inkhibit_framebuffer_tests PRIVATE inkhibit_framebuffer)
target_compile_features(inkhibit_framebuffer_tests PRIVATE cxx_std_17)
set_target_properties(inkhibit_framebuffer_tests PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_framebuffer_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

set(framebuffer_cases
    default_fill color_fills invalid_fill_colors corners orientation_marker
    neighbor_preservation color_pairs full_canvas invalid_coordinates
    invalid_buffer_sizes invalid_pixel_colors
)
foreach(case IN LISTS framebuffer_cases)
    add_test(NAME "framebuffer.${case}" COMMAND inkhibit_framebuffer_tests "${case}")
    set_tests_properties("framebuffer.${case}" PROPERTIES TIMEOUT 20)
endforeach()

# Real JPEG/PNG decoding and image transforms, independent of display hardware.
add_executable(inkhibit_image_tests image_tests.cpp)
target_link_libraries(inkhibit_image_tests PRIVATE inkhibit_image inkhibit_framebuffer)
target_compile_features(inkhibit_image_tests PRIVATE cxx_std_17)
set_target_properties(inkhibit_image_tests PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_image_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

set(image_cases
    decode.png_rgb decode.png_alpha decode.png_gray decode.jpeg
    decode.invalid_input decode.unsupported_format decode.side_limit
    decode.pixel_limit decode.truncated_png decode.file_png decode.file_errors
    decode.file_limit
    fit.exact fit.small fit.horizontal fit.vertical fit.both fit.mixed
    fit.fractional fit.thin_vertical fit.thin_horizontal fit.invalid
    color.palette color.hues color.tie pipeline_orientation
)
foreach(case IN LISTS image_cases)
    add_test(NAME "image.${case}" COMMAND inkhibit_image_tests "${case}")
    set_tests_properties("image.${case}" PROPERTIES TIMEOUT 20)
endforeach()

# Quantization exercises real palette selection, diffusion, and native packing.
add_executable(inkhibit_quantization_tests quantization_tests.cpp)
target_link_libraries(inkhibit_quantization_tests PRIVATE inkhibit_image)
target_compile_features(inkhibit_quantization_tests PRIVATE cxx_std_17)
set_target_properties(inkhibit_quantization_tests PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_quantization_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

set(quantization_cases
    nearest_compatibility palette_fills orientation
    diffusion_right diffusion_below_left diffusion_below diffusion_below_right
    signed_error clipping known_patch edge_left edge_right bottom_right
    default_method repeatability invalid_dimensions invalid_buffer_sizes
    invalid_method
)
foreach(case IN LISTS quantization_cases)
    add_test(NAME "quantization.${case}" COMMAND inkhibit_quantization_tests "${case}")
    # Some cases convert several full canvases; allow for Debug builds on Pi.
    set_tests_properties("quantization.${case}" PROPERTIES TIMEOUT 60)
endforeach()

# Exercise preview unpacking/file output and the actual hardware-free CLI.
add_executable(inkhibit_preview_tests preview_tests.cpp)
target_link_libraries(inkhibit_preview_tests PRIVATE inkhibit_image)
target_compile_features(inkhibit_preview_tests PRIVATE cxx_std_17)
set_target_properties(inkhibit_preview_tests PROPERTIES CXX_EXTENSIONS OFF)
add_dependencies(inkhibit_preview_tests inkhibit-preview)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_preview_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

set(preview_cases
    palette_fills nibble_pairs orientation invalid_sizes invalid_codes round_trip
    ppm_contents ppm_overwrite ppm_invalid_input ppm_open_error ppm_write_error
    cli_help cli_usage cli_invalid_method cli_nearest cli_floyd_and_default
    cli_input_errors cli_output_errors
)
foreach(case IN LISTS preview_cases)
    add_test(NAME "preview.${case}" COMMAND inkhibit_preview_tests
        "${case}" "$<TARGET_FILE:inkhibit-preview>")
    set_tests_properties("preview.${case}" PROPERTIES TIMEOUT 60)
endforeach()
