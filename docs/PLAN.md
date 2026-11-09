# Inkhibit — Project Plan

This document preserves the project requirements and records the initial
Milestone 1 review separately. It does not claim that hardware validation has
been completed.

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

## Planned technology stack

- C++17 or C++20; final choice remains open.
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
this phase. Begin with Milestone 1 and progress incrementally.

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
`94338de`. They are review observations, not completed hardware tests.

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
They are instructions only; they have not been executed against the hardware.

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

### Validation status and next step

- Checked panel source passes a host syntax-only compilation with the Inkhibit
  checking path enabled.
- Full hardware build was not validated in this environment; `lgpio.h` is absent.
- No physical hardware tests have run.
- Milestone 1 remains incomplete.

Next: fix GPIO linkage and the unchecked-build typo, add a single-refresh
validation executable, then verify colors, orientation, BUSY completion, sleep,
and shutdown on the Raspberry Pi before moving to Milestone 2.
