cmake_minimum_required(VERSION 3.18)

project(Inkhibit VERSION 0.1.0 LANGUAGES C CXX)

option(
    INKHIBIT_BUILD_HARDWARE
    "Build the Raspberry Pi lgpio display driver"
    OFF
)

set(WAVESHARE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/waveshare/lib"
)

add_library(inkhibit_framebuffer STATIC
    src/framebuffer.cpp
)

target_compile_features(inkhibit_framebuffer PUBLIC cxx_std_17)

set_target_properties(inkhibit_framebuffer PROPERTIES
    CXX_EXTENSIONS OFF
)

target_include_directories(inkhibit_framebuffer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_framebuffer PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

if(INKHIBIT_BUILD_HARDWARE)
    find_path(LGPIO_INCLUDE_DIR NAMES lgpio.h REQUIRED)
    find_library(LGPIO_LIBRARY NAMES lgpio REQUIRED)

    add_library(inkhibit_driver STATIC
        src/waveshare_lgpio.c
        "${WAVESHARE_DIR}/e-Paper/EPD_4in0e.c"
    )

    target_compile_features(inkhibit_driver PRIVATE c_std_11)

    set_target_properties(inkhibit_driver PROPERTIES
        C_EXTENSIONS OFF
    )

    target_compile_definitions(inkhibit_driver PRIVATE
        _POSIX_C_SOURCE=200809L
        RPI=1
        USE_LGPIO_LIB=1
        INKHIBIT_CHECKED_DRIVER=1
    )

    target_include_directories(inkhibit_driver
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE
            "${WAVESHARE_DIR}/Config"
            "${WAVESHARE_DIR}/e-Paper"
            "${LGPIO_INCLUDE_DIR}"
    )

    target_link_libraries(inkhibit_driver PUBLIC
        "${LGPIO_LIBRARY}"
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(inkhibit_driver PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_library(inkhibit_display STATIC
        src/display.cpp
        src/display_guard.cpp
    )

    target_compile_features(inkhibit_display PUBLIC cxx_std_17)

    set_target_properties(inkhibit_display PROPERTIES
        CXX_EXTENSIONS OFF
    )

    target_compile_definitions(inkhibit_display PRIVATE
        _POSIX_C_SOURCE=200809L
    )

    target_include_directories(inkhibit_display
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE
            "${WAVESHARE_DIR}/Config"
            "${WAVESHARE_DIR}/e-Paper"
    )

    target_link_libraries(inkhibit_display PUBLIC
        inkhibit_driver
        inkhibit_framebuffer
    )

    add_executable(inkhibit-display-test
        src/display_test.cpp
    )

    target_link_libraries(inkhibit-display-test PRIVATE
        inkhibit_display
    )

    set_target_properties(inkhibit-display-test PROPERTIES
        CXX_EXTENSIONS OFF
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(inkhibit_display PRIVATE
            -Wall -Wextra -Wpedantic
        )

        target_compile_options(inkhibit-display-test PRIVATE
            -Wall -Wextra -Wpedantic
        )
    endif()
endif()

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
