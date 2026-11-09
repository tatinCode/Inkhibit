# Inkhibit

Inkhibit is a Raspberry Pi-powered, six-color e-ink art frame. The goal is to
convert photographs and artwork for the display and manage a gallery through a
self-hosted web interface.

## Current status

The display foundation is working on a **Raspberry Pi Zero 2 W** running
**Raspberry Pi OS Lite 64-bit**, with a **Waveshare 4-inch e-Paper HAT+ (E),
Spectra 6**, using the model-specific `EPD_4in0e` driver.

- The maintainer has confirmed the six-color test pattern and corner L marker
  on the physical panel.
- The C++17 display wrapper validates framebuffers, performs one update per
  session, sleeps the panel, and releases GPIO/SPI resources.
- Exclusive process locking and a persistent 180-second cooldown are implemented.
- All **76 CTest cases pass** on the development machine and, as reported by the
  maintainer, on the Pi.
- The next milestone is the native JPG/PNG image-processing pipeline. The
  `inkhibit-display painting.jpg` command is a planned Phase 1 deliverable.

A browser-only gallery prototype is available in [web/](web/README.md). Its
gallery and previews run locally in the browser; integration with the Pi backend
is a later phase.

See [the project plan](docs/PLAN.md#current-progress) for milestone status and
the next implementation steps.

## Automated tests (no hardware required)

Requirements: Linux, CMake 3.18 or newer, a C11 compiler, and a C++17 compiler.

Run from the repository root on Arch Linux or Raspberry Pi OS:

```sh
cmake -S . -B build-tests \
  -DINKHIBIT_BUILD_HARDWARE=OFF \
  -DBUILD_TESTING=ON &&
cmake --build build-tests --parallel 2 &&
ctest --test-dir build-tests --output-on-failure
```

The tests compile the actual adapter, panel driver, and C++ wrapper against fake
lgpio functions and simulated monotonic/boot clocks. They require no lgpio
installation, root permissions, connected display, or real sleeps. Each CTest
case runs in its own process; lock and cooldown tests also launch independent
child processes with temporary state directories.

Coverage includes:

- Resource lifecycle and all seven initialization-failure stages.
- GPIO/SPI errors, bounded BUSY waits, interrupted sleeps, and first-error
  preservation through cleanup.
- Kernel-owned chip select, framebuffer validation and transmission, refresh
  sequencing, deep sleep, and one-shot wrapper behavior.
- Process locking, persistent cooldowns, interrupted sessions, boot changes,
  malformed and negative timestamps, and invalid state-file paths.

The unchecked vendor branch and the C++ sources also receive compile-only
checks. CTest uses fake hardware even when the hardware targets are enabled;
the physical smoke test below is a separate manual operation.

## Build and test on the Raspberry Pi

### Prepare the Pi

Install the build tools and lgpio development package:

```sh
sudo apt update
sudo apt install build-essential cmake liblgpio-dev
```

Enable SPI through `sudo raspi-config` under **Interface Options**, reboot if
required, and verify the device nodes:

```sh
ls -l /dev/gpiochip0 /dev/spidev0.0
```

The adapter uses gpiochip 0 and SPI bus 0, device 0, at 10 MHz in mode 0.

### Configure, compile, and run automated tests

From the repository root on the Pi:

```sh
cmake -S . -B build \
  -DINKHIBIT_BUILD_HARDWARE=ON \
  -DBUILD_TESTING=ON &&
cmake --build build --parallel 2 &&
ctest --test-dir build --output-on-failure
```

Two build jobs suit the Zero 2 W's 512 MB RAM; use `--parallel 1` if memory is
tight. Set `-DBUILD_TESTING=OFF` to omit test targets and compile checks.

`cmake -S . -B build` alone only configures the project. Hardware support defaults
to OFF in a fresh build directory; subsequent configurations reuse cached
settings unless explicitly overridden. Building and running CTest do not refresh
the panel.

### Run the physical smoke test

After a successful build, view the usage text without accessing the hardware:

```sh
./build/inkhibit-display-test --help
```

Perform one physical update:

```sh
sudo ./build/inkhibit-display-test
```

Expected result in landscape orientation:

- Bars from left to right: **black, white, yellow, red, blue, green**.
- A black outer border and white inner border.
- A white square containing an upright black **L** in the upper-left corner.

Flickering during the e-ink refresh is normal. On successful shutdown, the
program prints:

```text
Update complete. Panel slept and resources were released.
```

An immediate second invocation should report `Display cooldown active` and exit
without updating the panel. Allow at least **180 seconds after successful
shutdown** before another refresh. During an update, SIGINT/SIGTERM requests
allow the panel operation and cleanup to finish before the program exits.

## GPIO ownership and persistent state

GPIO numbers below use BCM numbering:

| Signal | GPIO | Owner |
| --- | --- | --- |
| RST | 17 | Adapter output |
| DC | 25 | Adapter output |
| PWR | 18 | Adapter output |
| BUSY | 24 | Adapter input; LOW means busy, HIGH means ready |
| CE0 / CS | 8 | Kernel SPI driver |
| MOSI / SCLK | 10 / 11 | SPI peripheral |

**The kernel controls CE0 around SPI transfers.** The adapter leaves GPIO 8
unclaimed and treats the vendor's manual CS writes as no-ops. On the validated
Pi configuration, `gpioinfo` reports GPIO 8 with consumer `spi0 CS0`; this is
expected. Trying to claim that line as an ordinary output caused the original
`GPIO busy (lgpio error -79)` failure.

Sessions use `/var/lib/inkhibit/` by default:

- `display.lock`: a nonblocking exclusive `flock` prevents concurrent sessions.
- `display.state`: a boot-ID-tagged active/idle record and boot-clock deadline,
  persisted through an atomic file replacement.

An unfinished session, invalid state record, or changed boot ID records a
**180-second recovery cooldown** and exits before hardware initialization.
Subsequent attempts observe the same deadline rather than restarting it. State
timestamps are parsed strictly with `std::from_chars`, including rejection of
negative values. Oversized or nonregular state files are rejected before
hardware initialization.

With no saved state, the guard waits until system uptime reaches 180 seconds
before allowing the first update. Preserve the state directory between runs so
the cooldown remains effective across separate invocations.

## Next milestone: image processing

First extract the tested landscape-to-native mapping and pixel-packing code
into hardware-independent framebuffer helpers. Then add JPG/PNG loading,
resizing/cropping, nearest-color conversion, and Floyd–Steinberg dithering, with
PC tests and preview output.

The target is a 600 × 400 landscape image mapped into the driver's 400 × 600
native layout: four bits per pixel, high nibble first, totaling **120,000 bytes**.
See [docs/PLAN.md](docs/PLAN.md) for the framebuffer contract and Phase 1
definition of done.
