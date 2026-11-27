# Inkhibit — Project Plan

This document records current progress and project requirements, followed by
the historical Milestone 1 review. Current validation includes automated tests
and maintainer-reported results on the Raspberry Pi. The historical review
describes an earlier repository state; its open issues and validation status
are superseded by the current progress below.

## Project overview

Inkhibit is an open-source, self-hosted digital art frame powered by a Raspberry
Pi. Users will upload photographs and artwork through a local web interface.
Images will be optimized for a six-color e-ink display housed in a 3D-printed,
museum-style picture frame.

The finished project should be lightweight, maintainable, modular, and
resource-efficient.

## Hardware

- Computer: Raspberry Pi Zero 2 W, 512 MB RAM.
- Operating system: Raspberry Pi OS Lite 64-bit.
- Display: Waveshare 4-inch e-Paper HAT+ (E), Spectra 6 (E6).
- Advertised resolution: 600 × 400 pixels.
- Interface: SPI.
- Colors: black, white, red, yellow, blue, and green.
- Development computer: Arch Linux.

Use documentation and the official C driver specifically for this 4-inch model.
Do not substitute drivers or assumptions from other Waveshare displays.

Official manual:
<https://www.waveshare.com/wiki/4inch_e-Paper_HAT%2B_%28E%29_Manual>

Official model-specific demo archive:
<https://files.waveshare.com/wiki/4inch-e-Paper-HAT%2B-(E)/4inch_e-Paper_E.zip>

## Technology stack

- C++17 for the display wrapper and C11 for the checked adapter and driver tests.
- CMake.
- Waveshare's existing C display driver.
- Vendored stb_image v2.30 for JPEG and PNG decoding; native C++ for orientation,
  canvas fitting, dithering, and framebuffer packing.
- Vendored cpp-httplib v0.67.0 (header-only) for the HTTP server.
- HTML, CSS, and vanilla JavaScript for the eventual frontend.
- NetworkManager for eventual Wi-Fi provisioning.
- Avahi/mDNS for eventual access through `inkhibit.local`.
- systemd for automatic startup.

Avoid unnecessary dependencies, heavy frameworks, and microservices.

## Phase 1 scope — Display integration

Create a standalone C++ program that displays ordinary JPG and PNG images on the
Waveshare screen.

Phase 1 is complete. The current phase connects the browser gallery in `web/`
to the frame through a local C++ HTTP server, in the small steps listed under
[Phase 2](#phase-2--web-gallery-integration). Wi-Fi provisioning remains a
later phase.

## Current progress

| Milestone | Status |
| --- | --- |
| 1. Hardware and driver validation | Complete for the single-refresh test-pattern foundation; Pi smoke test reported successful by the maintainer. |
| 2. C++ display interface | Complete; wrapper, lifecycle/error handling, process lock, and persistent cooldown implemented and tested. |
| 3. Automatic image processing | Implemented: JPEG/PNG decoding, JPEG EXIF orientation, aspect-preserving fitting, and six-color quantization and packing. |
| 4. Command-line image application | The core Phase 1 workflow is implemented and has been used on the panel. Both the display and hardware-free preview commands support dithering selection. |

The core Phase 1 deliverable works: `inkhibit-display painting.jpg` converts
an image, refreshes the panel once, and puts it to sleep. Phase 1 is complete.

### Phase 2 — Web gallery integration

| Step | Status |
| --- | --- |
| 1. Serve the gallery (`inkhibit-server`) | Done. All tests pass on the development machine and the Pi, and the gallery opens from other devices on the network. |
| 2. Persistent album storage and add/list/remove API (`web-album`) | Implemented; storage and HTTP tests pass on PC and Pi. |
| 3. Timed album rotation | Next: configurable elapsed interval, 30 minutes or longer. |
| 4. Browser album controls, status, authentication, systemd service | Planned. |

The main workflow is an album stored on the Pi that rotates without a browser
remaining open, not a one-off `POST /api/display` action. An optional "Show now"
action can be considered later.

Step 1 decisions:

- Server code lives in `include/inkhibit/server/` and `src/server/`, separate
  from the served `web/` directory, so backend files can never be downloaded.
- `configure_server()` mounts the web root and registers `/api/health` on a
  caller-owned `httplib::Server`, so tests and `main()` share the same setup.
- Sockets use `SO_REUSEADDR` instead of cpp-httplib's default `SO_REUSEPORT`:
  a restart can reuse the port immediately, but a second server cannot listen
  on the same port.
- The server binds to `0.0.0.0:8080` by default and prints the mDNS hostname,
  each IPv4 address, and `127.0.0.1`. It has no authentication yet;
  `--bind 127.0.0.1` restricts it to the local machine.
- SIGINT and SIGTERM are blocked in every thread and received by one waiter
  thread with `sigtimedwait`, which stops the server cleanly.

Album decisions:

- `include/inkhibit/album/` and `src/album/` implement validation and disk storage.
  Uploads are PNGs that decode to 600 × 400, limited to 2 MiB. Palette enforcement,
  dithering, and display refresh are not part of storage.
- `--album-root DIR` opts into `GET`/`POST /api/album` and
  `DELETE /api/album/<id>`. Storage must be outside the served web root.
  Files are saved unchanged under generated numeric IDs; list order is numeric
  insertion order. JSON IDs are strings to preserve uint64 precision.
- The counter survives deletion/restarts. Failed writes may leave gaps.
  Temporary-file renames hide incomplete PNG writes, but are not a guarantee
  against sudden power loss. One store instance owns a private directory;
  multiple processes writing that directory are unsupported.
- The CLI does not require loopback binding for album mode. The current API
  checks `Host: 127.0.0.1:<port>`, so normal LAN-hostname album requests receive
  403. This is not authentication; clients can supply the accepted Host header.
  Use `--bind 127.0.0.1` while testing the unauthenticated API.
- The gallery UI is not connected to this API. No rotation, per-image download
  route, quotas, or authentication is implemented in this increment.

### Validation checkpoint

- The maintainer built the hardware targets on the Pi and confirmed the
  six-color bars and upper-left L marker on the actual panel, then reported the
  setup working after the chip-select correction.
- All **203 CTest cases pass** on the development machine and, as reported by
  the maintainer, on the Raspberry Pi, including the nine album HTTP cases.
  The suite includes eight EXIF, nine
  cooldown, thirteen album-library, and twenty-four server cases. Automated hardware tests
  use fakes and simulated clocks; physical validation is separate.
- The maintainer generated previews with both methods and verified dithered
  image output, automatic cooldown waiting, and Ctrl+C cancellation on the Pi.
  The maintainer also displayed JPEGs with non-normal EXIF orientation and
  confirmed that portrait photos appear upright, matching their previews.
- The checked C adapter, model-specific panel driver, and C++ wrapper are tested
  together. The unchecked vendor branch and normal C++ sources have additional
  compile-only checks.
- Successful wrapper shutdown sends deep sleep, releases resources, and records
  the next permitted refresh time. The smoke-test completion message and
  immediate cooldown-rejection check are documented in the
  [README](../README.md#run-the-physical-smoke-test).

### Implemented display decisions

- `src/waveshare_lgpio.c` supplies the vendor-compatible GPIO symbols directly;
  the former `src/pi_config.h` has been removed.
- The adapter manually owns only RST (GPIO 17), DC (25), PWR (18), and BUSY (24).
  Kernel-managed CE0 on GPIO 8 selects the panel during SPI transfers through
  `/dev/spidev0.0`, at 10 MHz in mode 0. Vendor manual CS writes are no-ops in
  this adapter after the readiness check.
- Physical diagnostics showed GPIO 8 owned by `spi0 CS0`. The original manual
  claim conflicted with that kernel owner and produced lgpio error `-79`.
  Fake hardware now models this ownership and rejects manual CS access.
- Resource tests cover seven initialization stages and four GPIO frees. The
  suite includes regression cases for kernel-managed CS and CS calls before
  initialization.
- The checked driver preserves the first error through cleanup and applies a
  60-second engineering timeout to BUSY waits. Its deep-sleep path includes the
  two-second delay before resource release.
- `Display` and `DisplayGuard` are non-copyable and non-movable. `Display`
  initializes hardware lazily in `show()`, validates the full 120,000-byte
  framebuffer and every color nibble before hardware access, and permits one
  hardware-update attempt per object.
- `DisplayGuard` serializes sessions with `flock` and persists active/idle state
  and cooldown deadlines in `/var/lib/inkhibit/`. A successful shutdown starts
  a 180-second cooldown. An unfinished session, invalid state record, or changed
  boot ID records a fresh 180-second recovery cooldown on the next invocation.
  Repeated attempts do not extend that recovery deadline. With no state file,
  the first update requires at least 180 seconds of system uptime.
- `inkhibit-display` reports the remaining cooldown and waits without holding
  the display lock. It then opens a fresh session and rechecks the deadline.
  Ctrl+C cancels a pending update. The pattern-test command still exits when a
  cooldown is active.
- State deadlines use `CLOCK_BOOTTIME` and the Linux boot ID. Strict
  `std::from_chars` parsing rejects negative, overflowing, and partially numeric
  timestamps. Oversized or nonregular state files are rejected before hardware
  access.
- The working landscape mapping is `native_x = 399 - landscape_y` and
  `native_y = landscape_x`, packed in native row-major order with the first
  pixel in the high nibble.

### Implemented image processing decisions

- The file loader checks the reported file size against a 64,000,000-byte limit.
  Before full decode, the loader checks for at most 8,192 pixels on either axis
  and at most 33,000,000 pixels in total.
- JPEG EXIF correction handles all eight orientations in either byte order
  before resizing. The decoder reads the main image's orientation within the
  EXIF segment and keeps the stored pixel order if the tag is absent or unusable.
- Images are downscaled with nearest-neighbor sampling as needed and centered
  on a white 600 × 400 canvas without stretching or upscaling. Transparent PNG
  pixels are composited over white. Crop-to-fill is a possible later option.
- Nearest-color conversion and Floyd–Steinberg dithering share the same
  six-color palette. Floyd–Steinberg is the CLI default and uses two rolling
  rows of RGB error values.
- The reusable framebuffer module packs the landscape canvas into the driver's
  400 × 600 native layout: two pixels per byte, high nibble first, totaling
  120,000 bytes. `inkhibit-preview` expands this same packed buffer to a binary
  P6 PPM using RGB approximations of the panel colors.

### Next implementation increment

The 203-case suite has passed on the Pi, including the real-server album tests.
A manual upload/restart/delete smoke test remains optional; it is not required
to begin the next increment.

Implement timed album rotation through the existing image pipeline and
display guard. Intervals are elapsed durations, from 30 minutes to multiple
hours, not fixed times of day. The proposed default is one hour; no maximum
interval has been chosen. Persist the interval across restarts, never interrupt
an active refresh, and keep the existing hardware cooldown as an independent
safety constraint.

Before rotation implementation, settle startup/resume behavior, empty-album and
single-image behavior, how deletions affect the current image, and hardware/state
directory permissions for a non-root service account. Authentication and browser
integration remain later work; no unauthenticated public deployment is intended.

## Phase 1 milestones

### Milestone 1: Hardware and driver validation

1. Inspect the existing project structure.
2. Review the official Waveshare documentation and model-specific C driver.
3. Identify the correct driver files, GPIO configuration, SPI requirements, and
   initialization procedures.
4. Explain how to compile and run the official demo on the Raspberry Pi, while
   respecting the project's hardware-safety requirements.
5. Verify the driver API and framebuffer format from actual source code.
6. Preserve original driver functionality wherever possible.
7. Validate correct colors, orientation, refresh completion, sleep, and shutdown
   on the actual hardware before declaring the display foundation working.

### Milestone 2: C++ display interface

Create a modular C++ wrapper around the official C driver providing:

- Display initialization.
- Image/framebuffer transmission and full-screen refresh.
- Sleep and cleanup.
- Error reporting and bounded BUSY waits.
- Safe handling of repeated initialization.

Use RAII where appropriate and avoid unnecessary allocations. Keep C/C++ linkage
explicit. Do not invent vendor functions or assume that transmission and refresh
are separate operations in the existing API.

### Milestone 3: Automatic image processing

Create a hardware-independent pipeline that:

1. Loads ordinary JPG and PNG files.
2. Validates dimensions and file integrity.
3. Normalizes JPEG EXIF orientation and fits images inside a 600 × 400 landscape
   canvas, preserving aspect ratio.
4. Converts colors to the supported six-color palette.
5. Supports nearest-color quantization.
6. Implements Floyd–Steinberg dithering.
7. Maps orientation and packs pixels into the exact native framebuffer expected
   by the official driver.
8. Releases unnecessary memory after processing.

Aim for the best practical image quality within the hardware's limitations.
Image processing must be testable on Arch Linux without physical hardware.

### Milestone 4: Command-line application

Implemented interfaces (from the repository root after building):

```sh
sudo ./build/inkhibit-display painting.jpg
./build-tests/inkhibit-preview painting.jpg preview.ppm
```

Both commands accept `--dither nearest` or `--dither floyd-steinberg` before the
image path. The display command requires the Pi hardware build; the preview
command also builds without hardware support. See the [README examples](../README.md#display-an-image-or-save-a-preview).
Preserve the basic one-image display command when adding options such as
crop-to-fill.

## Hardware safety

- Respect the manufacturer's recommended refresh interval of at least 180 seconds.
- Do not repeatedly refresh the physical display during automated tests.
- Initialize and put the panel to sleep using the model-specific procedures.
- Never guess color encoding, pixel packing, or BUSY polarity.
- Keep hardware tests explicitly opt-in.
- Do not run a multi-refresh vendor demo unchanged when its delays conflict with
  the 180-second requirement.

## Quality requirements

- Favor clarity and maintainability over unnecessary abstraction.
- Minimize memory use on the 512 MB Pi.
- Avoid multiple full-resolution image copies where practical.
- Use CMake for building.
- Separate display-dependent code from image processing.
- Include meaningful error handling.
- Unit-test resizing, palette conversion, dithering, orientation mapping, and
  framebuffer generation without physical hardware.
- Do not introduce unnecessary third-party frameworks.

## Development approach

- Work incrementally rather than generating the entire application at once.
- Establish the display foundation before implementing the image pipeline.
- Explain important memory-management, Linux hardware-interface, and C/C++
  integration decisions.
- Preserve vendor code as a reference and document local changes.
- Never claim hardware tests passed unless they actually ran successfully.

## Definition of done

Phase 1 is complete when:

```sh
sudo ./build/inkhibit-display painting.jpg
```

automatically converts an ordinary photograph into a compatible framebuffer,
displays it correctly on the Waveshare 4-inch Spectra 6 panel, and safely puts
the panel to sleep afterward.

## Initial Milestone 1 review — 2026-10-29

These findings describe the repository on branch `e-paper-driver` at commit
`94338de`. They are historical review observations, not the current validation
checkpoint. The linkage issue, unchecked-build typo, single-refresh executable,
and display wrapper described as follow-up work here have since been addressed;
see [Current progress](#current-progress).

### Source-verified display details

The official model-specific archive was inspected in memory. Its
`EPD_4in0e.h` and `EPD_4in0e.c` confirm the driver identity and the following
native framebuffer contract. The checked-in `GUI_Paint.c` confirms nibble order.

- Native driver dimensions: **400 × 600**, despite the product's advertised
  600 × 400 landscape dimensions.
- Native row-major layout, 200 bytes per row.
- Four bits per pixel, two pixels per byte.
- First pixel in the high nibble, second in the low nibble.
- Framebuffer size: **120,000 bytes**.
- Landscape processing therefore needs an explicit rotation/orientation mapping;
  its direction must be verified on hardware with an asymmetric pattern.

| Color | Driver value |
| --- | --- |
| Black | `0x0` |
| White | `0x1` |
| Yellow | `0x2` |
| Red | `0x3` |
| Blue | `0x5` |
| Green | `0x6` |

The wiki's programming-principles color table conflicts with the model-specific
source. Use the model-specific driver values above, followed by physical color
validation. For example, yellow followed by red packs as `0x23`.

### GPIO and SPI

| Signal | BCM GPIO | Physical pin |
| --- | --- | --- |
| MOSI | 10 | 19 |
| SCLK | 11 | 23 |
| CS | 8 | 24 |
| DC | 25 | 22 |
| RESET | 17 | 11 |
| BUSY | 24 | 18 |
| PWR | 18 | 12 |

- Official lgpio SPI settings: bus 0, device 0, 10 MHz, mode 0.
- BUSY polarity: LOW means busy; HIGH means idle.
- Verify GPIO chip selection and chip-select behavior on the actual Pi and OS.

### Driver lifecycle

```text
DEV_Module_Init()
EPD_4IN0E_Init()
EPD_4IN0E_Display(framebuffer)
EPD_4IN0E_Sleep()
allow at least 2 seconds before module shutdown
DEV_Module_Exit()
```

- `DEV_Module_Init()` returns a status; the panel functions above return `void`.
- `EPD_4IN0E_Display()` transmits the buffer and performs a full refresh, including
  the panel power-on and power-off sequence.
- `EPD_4IN0E_Clear()` also performs a physical refresh.
- There is no separate public refresh function in this driver.
- The official example waits two seconds after sleep before module shutdown.
  Inkhibit's checked-driver sleep path currently includes that delay internally;
  avoid adding an accidental duplicate delay in the wrapper.
- The custom 60-second BUSY timeout is an engineering limit, not a
  manufacturer-specified maximum.

### Repository issues to resolve before hardware validation

1. **GPIO linkage:** `DEV_Config.h` declares external GPIO variables, while
   `src/pi_config.h` supplies macros visible only to the custom backend.
   The CMake target excludes the vendor file defining those variables, leaving
   unresolved panel-driver references when an application links the driver.
   Provide consistent shared definitions and add an executable link test.
2. **Unchecked-build typo:** the unchecked BUSY path in
   `third_party/waveshare/lib/e-Paper/EPD_4in0e.c` uses `Debugug` instead of the
   official `Debug` macro.
3. **Demo safety:** the vendor example performs multiple refreshes separated by
   short delays. Preserve it as a reference, but create a separate single-refresh
   validation program or enforce safe intervals before physical execution.
4. **Cleanup:** the vendored official lgpio cleanup is largely commented out.
   The custom backend improves cleanup but still requires validation.

### Official demo build reference

Run these steps on the Raspberry Pi, not on the Arch development computer.
This is the original vendor-demo reference from the initial review. Use the
[README](../README.md#build-and-test-on-the-raspberry-pi) for the validated
Inkhibit build and single-refresh smoke-test workflow.

Enable SPI with `sudo raspi-config`, reboot, and verify `/dev/spidev0.0` exists.
On current Raspberry Pi OS, configuration is normally under
`/boot/firmware/config.txt`.

```sh
sudo apt update
sudo apt install build-essential liblgpio-dev wget unzip
wget 'https://files.waveshare.com/wiki/4inch-e-Paper-HAT%2B-(E)/4inch_e-Paper_E.zip'
unzip 4inch_e-Paper_E.zip
cd 4inch_e-Paper_E/RaspberryPi_JetsonNano/c
make -j2
```

The vendor's documented run command is `sudo ./epd`. Do not execute the unchanged
multi-refresh demo under this project's safety requirements. First prepare a
single-refresh validation program or enforce at least 180 seconds between
physical refreshes, with appropriate sleep and cleanup.

### Additional design decisions before later milestones

- Enforce the refresh interval across separate CLI invocations, not only within
  one process; serialize concurrent access to the physical display.
- Choose maximum input dimensions and decoded-memory limits before allocating
  large image buffers.
- Define PNG transparency compositing and JPEG orientation behavior.
- Choose the C++ standard, image library, preview output format, and color-distance
  calculation when starting the relevant milestones.
- Keep hardware-disabled builds usable for image-processing tests on Arch Linux.

### Validation status and next step at the initial review

- Checked panel source passed a host syntax-only compilation with the Inkhibit
  checking path enabled.
- A full hardware build had not been validated in the review environment;
  `lgpio.h` was absent there.
- No physical hardware test results were recorded in that review.
- Milestone 1 was incomplete at that point.

The next steps then were to fix GPIO linkage and the unchecked-build typo, add a
single-refresh validation executable, and validate the lifecycle on the Pi.
Those steps are now superseded by the status in
[Current progress](#current-progress).
