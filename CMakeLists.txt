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


##
#framebuffer
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


##
#image
add_library(inkhibit_image STATIC
    src/image.cpp
)

target_compile_features(inkhibit_image PUBLIC cxx_std_17)

set_target_properties(inkhibit_image PROPERTIES
    CXX_EXTENSIONS OFF
)

target_include_directories(inkhibit_image PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(inkhibit_image PUBLIC
    inkhibit_framebuffer
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_image PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()


find_package(Threads REQUIRED)


##
# album
add_library(inkhibit_album STATIC
    src/album/image.cpp
    src/album/store.cpp
    src/album/rotation.cpp
    src/album/rotation_store.cpp
    src/album/rotation_worker.cpp
    src/album/rotation_step.cpp
)

target_compile_features(inkhibit_album PUBLIC cxx_std_17)

set_target_properties(inkhibit_album PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(inkhibit_album PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(inkhibit_album PRIVATE inkhibit_image Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_album PRIVATE -Wall -Wextra -Wpedantic)
endif()


##
# preview image
add_executable(inkhibit-preview
    src/preview_image.cpp
)

target_link_libraries(inkhibit-preview PRIVATE
    inkhibit_image
)

set_target_properties(inkhibit-preview PROPERTIES
    CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit-preview PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

##
# cpp-httplib (vendored, header only)
add_library(inkhibit_httplib INTERFACE)

target_include_directories(inkhibit_httplib SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib"
)

target_link_libraries(inkhibit_httplib INTERFACE
    Threads::Threads
)

##
# server 
add_library(inkhibit_server STATIC
    src/server/server.cpp
)

target_compile_features(inkhibit_server PUBLIC cxx_std_17)

set_target_properties(inkhibit_server PROPERTIES
    CXX_EXTENSIONS OFF
)

target_include_directories(inkhibit_server PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(inkhibit_server PUBLIC
    inkhibit_httplib
)

add_executable(inkhibit-server
    src/server/main.cpp
)

target_link_libraries(inkhibit-server PRIVATE
    inkhibit_server
    inkhibit_album
)

set_target_properties(inkhibit-server PROPERTIES
    CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inkhibit_server PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(inkhibit-server PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()


##
#
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
        src/display_cooldown.cpp
        src/cooldown_wait.cpp
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

    add_library(inkhibit_rotation_display STATIC
        src/album/rotation_display.cpp
    )

    target_compile_features(inkhibit_rotation_display PUBLIC cxx_std_17)

    set_target_properties(inkhibit_rotation_display PROPERTIES
        CXX_EXTENSIONS OFF
    )

    target_include_directories(inkhibit_rotation_display PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_link_libraries(inkhibit_rotation_display PRIVATE
        inkhibit_display
        inkhibit_image
        Threads::Threads
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(inkhibit_rotation_display PRIVATE
            -Wall -Wextra -Wpedantic
        )

    endif()

    target_link_libraries(inkhibit-server PRIVATE
        inkhibit_rotation_display
    )

    target_compile_definitions(inkhibit-server PRIVATE
        INKHIBIT_HAS_DISPLAY=1
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

    add_executable(inkhibit-display
        src/display_image.cpp
    )

    target_compile_features(inkhibit-display PRIVATE cxx_std_17)

    set_target_properties(inkhibit-display PROPERTIES
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(inkhibit-display PRIVATE
        inkhibit_display
        inkhibit_image
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(inkhibit-display PRIVATE
            -Wall -Wextra -Wpedantic
        )
    endif()
endif()


include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
