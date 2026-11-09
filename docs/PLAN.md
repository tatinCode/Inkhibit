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
- A lightweight native image-processing library; selection remains open.
- cpp-httplib for the eventual HTTP server.
- HTML, CSS, and vanilla JavaScript for the eventual frontend.
- NetworkManager for eventual Wi-Fi provisioning.
- Avahi/mDNS for eventual access through `inkhibit.local`.
- systemd for automatic startup.

Avoid unnecessary dependencies, heavy frameworks, and microservices.

## Current scope: Phase 1 — Display integration

Create a standalone C++ program that displays ordinary JPG and PNG images on the
Waveshare screen.

Do not implement the HTTP server, Wi-Fi provisioning, or gallery interface in
this phase. Progress incrementally through the milestones below. The separate
browser-only prototype in `web/` is UI groundwork; backend integration belongs
to a later phase.

## Current progress

| Milestone | Status |
| --- | --- |
| 1. Hardware and driver validation | Complete for the single-refresh test-pattern foundation; Pi smoke test reported successful by the maintainer. |
| 2. C++ display interface | Complete; wrapper, lifecycle/error handling, process lock, and persistent cooldown implemented and tested. |
| 3. Automatic image processing | Next; extract framebuffer helpers before adding image loading and conversion. |
| 4. Command-line image application | Pending the image pipeline. |

Phase 1 remains in progress: `inkhibit-display-test` displays a generated
pattern, while the `inkhibit-display painting.jpg` image command in the
definition of done is still to be implemented.

### Validation checkpoint

- The maintainer built the hardware targets on the Pi and confirmed the
  six-color bars and upper-left L marker on the actual panel, then reported the
  setup working after the chip-select correction.
- All **76 CTest cases pass** on the development machine, with the updated suite
  also reported passing on the Pi. Automated tests use fake hardware and
  simulated clocks; physical validation is a separate manual operation.
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
- State deadlines use `CLOCK_BOOTTIME` and the Linux boot ID. Strict
  `std::from_chars` parsing rejects negative, overflowing, and partially numeric
  timestamps. Oversized or nonregular state files are rejected before hardware
  access.
- The working landscape mapping is `native_x = 399 - landscape_y` and
  `native_y = landscape_x`, packed in native row-major order with the first
  pixel in the high nibble.

### Next implementation increment

1. Extract landscape-to-native coordinate mapping and pixel packing from
   `src/display_test.cpp` into a reusable, hardware-independent module.
2. Test all corners, an asymmetric orientation marker, high/low nibble order,
   all six color codes, and the exact framebuffer size on the PC.
3. Select a lightweight JPG/PNG decoder and resizing approach, with explicit
   dimension and decoded-memory limits for the 512 MB Pi.
4. Add decoding, aspect-ratio-preserving resizing/cropping, and nearest-color
   quantization, followed by Floyd–Steinberg dithering and preview output.
5. Connect the image pipeline to `Display` through the image CLI in Milestone 4.

Proposed image defaults to confirm during Milestone 3: center-crop to fill a
600 × 400 landscape canvas, composite transparent PNG pixels onto white, and
normalize JPEG EXIF orientation before resizing. These are planned processing
policies, not implemented features.

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
3. Resizes and crops images to a 600 × 400 landscape canvas.
4. Converts colors to the supported six-color palette.
5. Supports nearest-color quantization.
6. Implements Floyd–Steinberg dithering.
7. Maps orientation and packs pixels into the exact native framebuffer expected
   by the official driver.
8. Releases unnecessary memory after processing.

Aim for the best practical image quality within the hardware's limitations.
Image processing must be testable on Arch Linux without physical hardware.

### Milestone 4: Command-line application

Basic interface:

```sh
./inkhibit-display painting.jpg
```

Eventually support:

- `--preview`: save a simulated e-ink preview without touching the display.
- `--dither`: select the dithering method.
- `--fit`: choose cropping or aspect-ratio-preserving fitting.
- `--display`: explicitly send processed artwork to the connected display.

Exact option syntax and interactions remain to be defined. Preserve the basic
one-image command required by the definition of done.

Handle missing files, invalid formats, incorrect arguments, and hardware errors
gracefully.

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
./inkhibit-display painting.jpg
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
Those steps are now superseded by the image-processing increment in
[Current progress](#current-progress).
