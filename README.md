# Inkhibit
a Raspberry Pi-powered digital art frame that transforms uploaded images into colorful e-ink artwork. Featuring a self-hosted web interface and image optimization, Inkhibit brings a miniature, customizable art gallery to your wall.

## Driver tests (no hardware required)

Run from the repository root on Arch Linux or Raspberry Pi OS:

```sh
cmake -S . -B build-tests -DINKHIBIT_BUILD_HARDWARE=OFF -DBUILD_TESTING=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The tests compile the actual GPIO/SPI adapter and 4-inch panel driver against
fake lgpio functions and a simulated monotonic clock. They require no lgpio
installation, root permissions, connected display, or real sleeps. Each case
runs in its own process to isolate the driver's static state.

Coverage includes resource lifecycle and partial initialization failures,
GPIO/SPI errors, BUSY deadlines, interrupted sleeps, error preservation,
120,000-byte framebuffer transmission, refresh command sequencing, and sleep.
The unchecked vendor branch also receives a compile-only check. These tests
validate software behavior; physical colors, orientation, and timing still
require separate manual validation on the panel.

Hardware library builds remain available with `-DINKHIBIT_BUILD_HARDWARE=ON`
and require the lgpio development package. Use `-DBUILD_TESTING=OFF` when only
the hardware library is wanted.
