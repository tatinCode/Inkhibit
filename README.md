# Inkhibit

Inkhibit is a Raspberry Pi-powered, six-color e-ink art frame. The goal is to
convert photographs and artwork for the display and manage a gallery through a
self-hosted web interface.

## Current status

The display foundation is working on a **Raspberry Pi Zero 2 W** running
**Raspberry Pi OS Lite 64-bit**, with a **Waveshare 4-inch e-Paper HAT+ (E),
Spectra 6**, using the model-specific `EPD_4in0e` driver.

- `inkhibit-display` shows JPEG and PNG images. It performs one update, puts
  the panel to sleep, releases GPIO/SPI resources, and starts a persistent
  180-second cooldown. A process lock prevents concurrent updates.
- `inkhibit-preview` saves a 600 × 400 PPM preview from the same framebuffer
  without accessing the panel.
- `inkhibit-server` serves the browser gallery prototype over HTTP, with a
  `/api/health` endpoint. It does not access the panel yet.
- The maintainer has verified the test pattern and dithered image display on
  the physical panel, including automatic cooldown waiting and cancellation.
  All **181 CTest cases pass** on the development machine.

The gallery prototype in [web/](web/README.md) still keeps its artwork and
previews in the browser. Sending an image from the gallery to the frame is the
next step.

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
- Automatic cooldown waiting, retry with a fresh display session, and
  cancellation without refreshing the panel.
- JPEG and PNG decoding, transparency, all eight JPEG EXIF orientations,
  canvas fitting, six-color quantization, and native framebuffer packing.
- Floyd–Steinberg dithering, PPM preview output, and preview CLI behavior.
- HTTP server health and static files, refusal of path traversal and symlink
  escapes, port conflicts and quick restarts, and server CLI options, startup
  URLs, and signal shutdown.

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
tight. Files that include the large `httplib.h` header need the most memory; if
the build is killed or the Pi stops responding, rerun it with `--parallel 1`. Set `-DBUILD_TESTING=OFF` to omit test targets and compile checks.

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

An immediate second invocation of `inkhibit-display-test` should report
`Display cooldown active` and exit without updating the panel. Allow at least
**180 seconds after successful shutdown** before another refresh. During an
update, SIGINT/SIGTERM requests allow the panel operation and cleanup to finish
before the program exits.

## Display an image or save a preview

After a hardware-enabled build on the Pi, display a JPEG or PNG from the
repository root. Run one of these commands:

```sh
# Floyd–Steinberg dithering is the default.
sudo ./build/inkhibit-display painting.jpg

# Use nearest-color conversion instead.
sudo ./build/inkhibit-display --dither nearest painting.png
```

If a cooldown is active, `inkhibit-display` reports the remaining time, waits
automatically, and checks again before refreshing. Press **Ctrl+C** while
waiting to cancel the pending update. If a refresh has already started, the
program lets the panel operation and cleanup finish before exiting.

Both commands fit the image inside the 600 × 400 landscape canvas, preserving
its aspect ratio without upscaling. Margins are white, and transparent PNG
pixels are composited over white. JPEGs are rotated or mirrored according to
their EXIF orientation before fitting; missing or unusable orientation
metadata leaves the stored pixel order intact.

To compare methods without a display, use the build from
[Automated tests](#automated-tests-no-hardware-required):

```sh
./build-tests/inkhibit-preview painting.jpg dithered.ppm
./build-tests/inkhibit-preview --dither nearest painting.jpg nearest.ppm
```

On the Pi, the hardware-enabled build also provides `./build/inkhibit-preview`.
Previewing needs no `sudo` and does not start a cooldown. The binary P6 PPM
uses RGB approximations of the six panel colors, so its colors may look
different from the panel's.

## Serve the web gallery

`inkhibit-server` serves the files in `web/` and answers `GET /api/health`
with `{"status":"ok"}`. It doesn't access the panel, so it needs no `sudo`.
From the repository root:

```sh
./build/inkhibit-server          # on the Pi
./build-tests/inkhibit-server    # on the development machine
```

On startup it lists addresses you can open, for example:

```text
Serving web on port 8080 (all network interfaces):
  http://bippy.local:8080/
  http://192.168.1.42:8080/
  http://127.0.0.1:8080/
Press Ctrl+C to stop.
```

Open the `.local` hostname from another device on the same network. Some
Android devices and some routers don't support `.local` names; use the
listed IP address instead. Press **Ctrl+C**, or send SIGTERM, to stop the
server.

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--bind ADDRESS` | `0.0.0.0` | Address to listen on. `0.0.0.0` accepts connections on every network interface. |
| `--port PORT` | `8080` | TCP port, 1–65535. |
| `--web-root DIR` | `web` | Directory to serve. Relative paths are resolved from the current directory. |

**The server is reachable by every device on your network and has no login
yet.** Run it only on a network you trust, or use `--bind 127.0.0.1` to accept
connections from the Pi itself only. Files are served only from inside the web
root: `..` paths and symlinks that point outside it are refused. A second
server cannot start on a port that is already in use.

The server stops when the SSH session that started it ends. Until a systemd
service is added, run it inside `tmux` on the Pi to keep it running after you
disconnect:

```sh
tmux new -s inkhibit
./build/inkhibit-server
# Detach with Ctrl+B, then D. Reattach later with: tmux attach -t inkhibit
```

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
**180-second recovery cooldown** before any hardware initialization.
Subsequent attempts observe the same deadline rather than restarting it. State
timestamps are parsed strictly with `std::from_chars`, including rejection of
negative values. Oversized or nonregular state files are rejected before
hardware initialization.

With no saved state, the guard rejects updates until system uptime reaches
180 seconds. The image command waits automatically; if the pattern test
reports a cooldown, retry it after the reported wait. Preserve the state
directory between runs so the cooldown remains effective across separate
invocations.
