# projectMM

Drive large LED installations and DMX fixtures. One source tree drives ESP32, Teensy, Raspberry Pi, macOS, Windows and Linux.

![Web UI](docs/assets/ui/ui_theme.gif)

👉 **Try it now:** flash an ESP32 straight from your browser → <https://moonmodules.org/projectMM/install/>. Step-by-step in the [Getting started guide](docs/gettingstarted.md).

📦 **Release + downloads:** [latest release](https://github.com/MoonModules/projectMM/releases/latest)

🛠️ **Building / hacking on it?** [MoonDeck](moondeck/MoonDeck.md), our browser-based dev console (build · flash · test · live device discovery), comes in the repo.

Open Chrome or Edge, plug in your device, and you'll see lights in under a minute.

If you like projectMM, give it a ⭐️, fork it, or open an issue or pull request; it helps the project grow, improve, and get noticed.

## What makes projectMM different

🔵 **16,384 LEDs on a *classic* ESP32**, not just the S3 or P4. Memory-adaptive from a 16×16 panel up to 128×128, degrading gracefully on tight devices instead of crashing.

🧊 **Native 3D from the ground up**: 2D and 1D are just the cases where a dimension is size 1. Effects never pick a mode.

🎛️ **Pluggable pipeline**: Layouts → Effects (layers of effects + modifiers) → Drivers. Build it visually in the browser, and every change applies live (settings also persist to flash across power cycles).

🔄 **No reboot to apply a configuration change**: edit a pin map, a strand length, an output protocol, or the mic on a running device and it takes effect on the very next frame, with no init-at-boot step, no restart. Where most LED-controller firmware needs a reboot for a pin or protocol change, projectMM applies it live. (Flashing new *firmware* over OTA still needs the usual power cycle, since that's a binary swap rather than a config change.)

💡 **DMX *and* addressable LEDs in one setup**: RGB strips, RGBW pixels, par lights, moving heads, all through the same pipeline.

🔌 **Parallel WS2812 output**: drive many strands at once over three ESP32 peripherals: RMT (every chip), the S3's LCD_CAM i80 bus (8 lanes), and the P4's Parlio engine (up to 8 lanes), each with an on-device loopback self-test that bit-verifies the wire signal.

🌐 **Industry protocols, both directions**: send *and* receive [Art-Net](https://art-net.org.uk/), [E1.31/sACN](https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf), and [DDP](http://www.3waylabs.com/ddp/) over the network, interoperable with Falcon, Advatek, xLights, Resolume, LedFx and other industry gear.

🎵 **Audio-reactive**: an I²S microphone drives a 16-band FFT spectrum + sound level, consumed by audio-reactive effects, all built fresh from the mic datasheet and textbook DSP.

🏠 **Home-automation control**: a device joins Homebridge (and any MQTT hub) over a dependency-free MQTT 3.1.1 client: on/off, brightness, and a HomeKit color wheel that picks the nearest palette. See [the MQTT module docs](docs/moonmodules/core/services.md#mqtt).

📁 **On-device File Manager**: browse and edit the device filesystem from the browser: a lazy folder tree with an inline editor, drag-drop upload, and create/delete, plus [firmware upload OTA](docs/moonmodules/core/services.md#firmware-update) (flash a `.bin` over the LAN, no USB). See [the File Manager docs](docs/moonmodules/core/services.md#file-manager).

🛡️ **Robust to any input**: add, delete, replace, or reconfigure any module in any order, at any grid size, and the device keeps running, degraded or idle, but never crashed. Every crash that's ever found becomes a regression test, so it stays fixed.

🖥️ **One source tree, many targets**: the same code runs on ESP32 (Xtensa and RISC-V), Teensy, Raspberry Pi, and macOS / Windows / Linux. On the desktop that means **arm64 and x86-64 alike**: MoonLive's script JIT has a native backend for each, so a script compiles to real machine code on Apple Silicon, on an Intel Mac, and on a Windows or Linux PC.

🎨 **Plug in, open a browser, see lights**: a live 3D preview of every effect, modifier, and layout, controllable from the same tab. The interface renders any module from its declared controls, so adding a module needs zero UI code.

🌗 **MoonBase, the second boot image**: instead of spending half the flash on a second firmware copy, a ~750 KB maintenance image sits in the factory slot and installs updates into one large app slot, one click in the UI covers the whole reboot-install-reboot cycle, and a power cut mid-update lands back in MoonBase, never in a half-written app. Forced on a 4 MB board, which has room for one application and not two, and chosen on the larger ones, where the freed slot goes to the filesystem instead. See [architecture.md § MoonBase](docs/architecture.md#moonbase-the-second-boot-image).

⚡ **Flash from your browser in seconds**: the web installer picks your device, flashes the matching firmware, and hands WiFi credentials to the device over USB via Improv. No serial monitor, no recompile.

## Under the hood

🛠️ **ESP-IDF directly, no Arduino**: the ESP32 build is pure ESP-IDF (v6.x): native LED drivers, `esp_http_server`, FreeRTOS, built with `idf.py`, not PlatformIO or the Arduino framework. See [building.md § Why not Arduino](docs/building.md#why-not-arduino).

📦 **No third-party libraries**: no FastLED, no ESPAsyncWebServer, no ArduinoJson. The color math, the HTTP/WebSocket server, and the control storage are all in-tree. A library, when genuinely needed, lives behind the platform boundary in `src/platform/`, never in core. The full rationale + replacements: [building.md § Third-party libraries](docs/building.md#third-party-libraries); why we take the trade at all: [Why we write our own code](docs/why-we-write-our-own.md).

🔬 **Industry standards, our own code**: we study the prior art hard (friend repos, peripheral datasheets, the Art-Net / E1.31 / WS2812 standards), carry its *ideas* forward, and credit it by name; but we write our own code rather than copying theirs or tracing their structure. Each feature is spec'd from the primary source, its behavior pinned with unit + scenario tests, then written fresh against our own architecture, so the result is independent by construction, not a renamed fork. Textbook algorithm, textbook name, our implementation. The method: [CLAUDE.md § Principles](CLAUDE.md#principles); how we tell good theft from bad: [Why we write our own code](docs/why-we-write-our-own.md#good-theft-and-bad-theft).

🧱 **One module model**: every effect, modifier, layout, and driver is a `MoonModule`: one base class, a uniform lifecycle, declared controls. That uniformity is why the UI renders any module with zero per-module code, and why a new capability is a new file, not a new framework. See [architecture.md § MoonModules](docs/architecture.md#moonmodules).

## Performance

Measured end-to-end through a full render pipeline (effect → modifier → ArtNet output) on real hardware. FPS is derived from the per-frame tick time.

The **Desktop** column is host-CPU-bound, not OS-bound: the numbers track the machine, not macOS vs Windows vs Linux. Captured on Apple Silicon (M-series); the macOS and Windows binaries run the same code on comparable hardware.

### Frames per second

| Grid | Lights | Desktop | Olimex `esp32` | Olimex `esp32-eth` | LOLIN S3 N16R8 `esp32s3-n16r8` |
|---|---:|---:|---:|---:|---:|
| 16×16 | 256 | *(below host clock resolution)* | 1,543 | 1,628 | 1,672 |
| 32×32 | 1,024 | 166,667 | 447 | 432 | 287 |
| 64×64 | 4,096 | 40,000 | 81 | 71 | 25 |
| 128×128 | 16,384 | 9,708 | 11 | 10 | 6 |

The Olimex `esp32` figures were measured on the WiFi+Ethernet build (the pre-collapse `esp32-eth-wifi`, now the default `esp32`). The LOLIN S3 N16R8 was measured over WiFi with `Network.txPowerSetting` capped to 8 dBm (the brown-out fix, see below); at 128×128 it's bound by ArtNet over WiFi at reduced TX power (~93 ms of the ~164 ms tick), which is why it trails the Ethernet devices despite a faster core. (The S3 now also supports W5500 SPI Ethernet, which sidesteps that WiFi bottleneck on devices wired for it.) This device's niche is PSRAM headroom (8 MB) for large pixel buffers; use an Ethernet device when frame rate matters.

### Free heap

Each cell is **free internal RAM / largest contiguous internal-RAM block**. Internal RAM is the scarce, comparable resource across all devices, so for PSRAM devices (the S3) this is internal-only, NOT the PSRAM-merged total (we assume the 8 MB PSRAM pool is large enough that it isn't the constraint). The block size is the memory-pressure signal that matters: free RAM can be ample while fragmentation leaves no single block big enough for the next allocation.

| Grid | Desktop | Olimex `esp32` | Olimex `esp32-eth` | LOLIN S3 N16R8 `esp32s3-n16r8` |
|---|---:|---:|---:|---:|
| 16×16 | unlimited | 139 KB / 52 KB | 178 KB / 100 KB | 238 KB / 160 KB |
| 32×32 | unlimited | 132 KB / 50 KB | 172 KB / 92 KB | 240 KB / 152 KB |
| 64×64 | unlimited | 108 KB / 48 KB | 147 KB / 62 KB | 236 KB / 152 KB |
| 128×128 | unlimited | 129 KB / 52 KB | 132 KB / 48 KB | 240 KB / 164 KB |

The S3's internal-free stays flat across grid sizes because its Layer buffer + LUT live in PSRAM: growing the grid consumes PSRAM, not internal RAM. The Olimex devices hold those buffers in internal RAM, so their free heap drops as the grid grows.

Build variants differ structurally: the default `esp32` includes the WiFi stack (~270 KB flash, ~28 KB heap) alongside Ethernet. `esp32-eth` drops WiFi for more free heap, at the cost of slightly slower tick on large grids (lwIP buffer-pool sizing is tuned for the WiFi+Ethernet sdkconfig). The right variant depends on whether the deployment needs WiFi at all, or only Ethernet plus the extra buffers.

The numbers above are observations. The **contracts** projectMM commits to, what the device must hit on every CI run, live in [`test/scenarios/*.json`](test/scenarios/) as per-step `contract.<target>` blocks; see [docs/testing.md § Performance contracts](docs/testing.md#performance-contracts-contracttarget) for how they're set and renegotiated. The [docs/performance.md](docs/performance.md) page covers the *why* (WiFi vs Ethernet physics, sizeof tables, build-variant deltas).

## Getting started

### From a release

**ESP32: flash from your browser.** Open the [web installer](https://moonmodules.org/projectMM/install/) in Chrome or Edge; it walks you through release, device and firmware selection, flashing, and network setup. The installer lists stable releases and a `latest` build (published automatically on every merge to main) carrying the newest unreleased changes.

![Installer](docs/assets/ui/installer.png)

**Desktop: download and run.** Grab the build for your OS from the [releases page](https://github.com/MoonModules/projectMM/releases). Step-by-step with screenshots for Windows: [Installing projectMM on a desktop](docs/tutorials/installing-to-desktop.md).

- **macOS arm64:** `projectMM-macos-arm64-vX.Y.Z.dmg`: open it and drag projectMM to Applications, then launch it like any app. A Terminal window opens showing what it is doing, your browser opens the UI, and closing that window stops it. (`projectMM-macos-arm64-vX.Y.Z.tar.gz` is the same binary without the wrapper, for scripting.) x86-64 macOS is supported and tested, but only the arm64 build is packaged: build from source for an Intel Mac. The binary is ad-hoc signed rather than notarized, so Gatekeeper says it cannot verify the developer; right-click → Open and confirm, or clear the flag with `xattr -dr com.apple.quarantine ./projectMM`.
- **Windows x64:** `projectMM-windows-x64-vX.Y.Z-setup.exe`: run it and projectMM installs for your user (no admin prompt) with a Start-menu entry and an uninstaller. `projectMM-windows-x64-vX.Y.Z.zip` is the same binary to unzip and run from anywhere, and it carries `Install-projectMM.cmd` if you would rather install it from a script you can read (or if Defender blocks the setup download, which it occasionally does to an unsigned build). Neither is code-signed, so SmartScreen asks you to confirm once: your browser flags the download ("isn't commonly downloaded"), and keeping it there is the trust decision. Walkthrough with screenshots: [Installing projectMM on a desktop](docs/tutorials/installing-to-desktop.md).
- **Linux x64:** `projectMM-linux-x64-vX.Y.Z.tar.gz`, or `projectmm_X.Y.Z_amd64.deb` on Debian, Ubuntu and Raspberry Pi OS (`sudo apt install ./projectmm_X.Y.Z_amd64.deb` puts it on your PATH).

Then open `http://localhost:8080/`. It opens by itself on start; pass `--no-browser` to suppress
that (a headless server, or a service manager), and `--port <n>` to serve somewhere else.

**Your settings live with your user, not beside the executable**, so they survive moving the app, reinstalling, and upgrading: `%LOCALAPPDATA%\projectMM` on Windows, `~/Library/Application Support/projectMM` on macOS, and `$XDG_DATA_HOME/projectMM` on Linux, falling back to `~/.local/share/projectMM` when that is unset. An uninstall leaves them in place; delete that folder to start clean. Set `MM_DATA_DIR` to put them somewhere else. Running from a source checkout keeps using `build/fs/` instead (config under `build/fs/.config/`), so a development tree stays self-contained.

Once running, the UI lets you build a render pipeline visually (layouts → layers with effects + modifiers → drivers), preview the result in 3D, send it to Art-Net, and save it. The source tree also builds for Teensy, Raspberry Pi, and Linux from source (see [building.md](docs/building.md)), though currently only the macOS, Windows, Linux and ESP32 binaries ship as releases.

### From source

You need [uv](https://docs.astral.sh/uv/) (Python launcher), CMake 3.20+, and a C++20 compiler. For ESP32, ESP-IDF v6.x is also required; see [building.md](docs/building.md) for the full setup instructions.

Once prerequisites are in place, launch MoonDeck, the browser-based dev console:

```sh
uv run moondeck/moondeck.py
```

Open `http://localhost:8420`: Desktop tab to build / run / test, ESP32 tab to flash, Live tab to discover devices. Full per-command reference: [moondeck/MoonDeck.md](moondeck/MoonDeck.md).

![Moondeck Desktop](docs/assets/ui/moondeck_desktop.png)

## Documentation

| Document | What's in it |
|----------|--------------|
| [architecture.md](docs/architecture.md) | How the system is put together: core runtime + light domain, pipeline, memory, parallelism |
| [coding-standards.md](docs/coding-standards.md) | How code in this repo is written: conventions, file shape, static checks |
| [building.md](docs/building.md) | How to build and flash for every supported target |
| [testing.md](docs/testing.md) | What tests exist and what they cover |
| [performance.md](docs/performance.md) | Per-module timing, memory, sizeof, per platform |
| [moonmodules/](docs/moonmodules/) | One spec page per module: [core](docs/moonmodules/core/) services and [light](docs/moonmodules/light/) effects, layouts, modifiers, drivers |
| [CLAUDE.md](CLAUDE.md) | Rules, constraints, and development process |

## How we work

projectMM is built by AI agents under tight human direction. Everything in this repository, firmware and desktop code, the web installer, the MoonDeck dev console, all documentation, the unit and scenario tests, even the UI screenshots and effect GIFs, is authored by agents; the **product owner** writes none of it directly. What the product owner *does* author is the **process** ([CLAUDE.md](CLAUDE.md)), the **architecture** ([architecture.md](docs/architecture.md)), and the **module specifications** ([docs/moonmodules/](docs/moonmodules/)); then decides what to build next, reviews every line and every spec, runs the hardware tests, and controls every commit, merge, and release. Agents write in defined roles; they don't make decisions. The agent writes; the product owner thinks.

Meet the team: 🤖 Architect designs, 👽 Developer implements, 👾 Reviewer checks before merge, 🛸 Tester verifies, 💀 Runner does quick build and check passes. Full team descriptions in [CLAUDE.md](CLAUDE.md).

A few principles run through everything:

- **Common patterns first**: recognisable practice across code, docs, tests, UI. Bespoke choices need a stated reason.
- **Specs before code**: a module is documented in [`docs/moonmodules/`](docs/moonmodules/), purpose, controls, behaviour, edge cases, prior art, well enough to implement from before it's written.
- **Working software at every commit**: each commit builds, passes the test + scenario gates, and produces something you can see run; never a broken intermediate state.
- **Minimalism**: flat, predictable code; removing code beats adding it; every addition pays for itself.
- **The system as it is**: code and docs describe the present; git history is the changelog.

The full rules and process are in [CLAUDE.md](CLAUDE.md).

## History

This is the current iteration of years of LED / light system development. Each prior project proved ideas this one builds on:

| Project | Description | Repo |
|---------|-------------|------|
| **WLED** | Open-source LED firmware (user / contributor since 2021) | [Aircoookie/WLED](https://github.com/Aircoookie/WLED) |
| **WLED-MoonModules** | WLED fork with advanced features | [MoonModules/WLED](https://github.com/MoonModules/WLED) |
| **StarLight** | Standalone LED firmware | [ewowi/StarLight](https://github.com/ewowi/StarLight) |
| **MoonLight** | Ground-up build: 60+ effects, memory-optimised mapping, 11 driver types | [ewowi/MoonLight](https://github.com/ewowi/MoonLight) |

We built, maintained, and contributed to these projects, so projectMM is grounded in years of our own hands-on experience, not arms-length study. Their lessons and proven patterns are distilled in [`docs/history/`](docs/history/README.md), alongside monthly digests of friend projects (like FastLED and upstream WLED) we follow closely but don't own. From all of it we carry the ideas forward into our own implementation: we apply what we learned and write our own code rather than copying theirs; and when a specific project or person inspires something here, we credit them by name (in the history digests and each module's "Prior art" notes).

## Credits

Specific people whose work directly shaped parts of projectMM. We study their thinking with respect and write our own code against our architecture rather than tracing theirs. These credits name the prior art behind a feature:

- **[WLED](https://github.com/wled/WLED) and [WLED-MM](https://github.com/MoonModules/WLED)**: projectMM is born out of WLED, and takes the usermod idea to a new level. Here *everything* is a mod (a MoonModule): effects, drivers, networking, the file system, the system manager. It also integrates tightly with WLED: a projectMM device can act as a WLED device, and it talks to WLED devices (audio sync, discovery, and more).
- **Frank ([softhack007](https://github.com/softhack007))**: main author of the WLED-MM audio-reactive usermod, the most-used open-source audio-reactive LED implementation. The ideas behind [AudioService](docs/moonmodules/core/moxygen/AudioService.md) (including the adaptive noise-gate concept, analyzed with his permission) descend from years of collaboration on WLED-SR / WLED-MM. He also inspired the [Flying Toasters](docs/moonmodules/light/effects.md#flyingtoasters) effect and the sprite support behind it.
- **[troyhacks](https://github.com/troyhacks/WLED)**: reworked the WLED-MM audio-reactive DSP to run on Espressif's [esp-dsp](https://github.com/espressif/esp-dsp) FFT (a low-latency, "stupid fast" alternative to ArduinoFFT); the same esp-dsp FFT choice [AudioService](docs/moonmodules/core/moxygen/AudioService.md) makes. See its Prior art notes.
- **[Stefan Petrick](https://github.com/StefanPetrick)**: the generative-field vocabulary the LED world learned from [Animartrix](https://github.com/StefanPetrick/animartrix), [FunkyNoise](https://github.com/StefanPetrick/FunkyNoise) and [ColorTrails](https://github.com/StefanPetrick/ColorTrails): noise read in polar coordinates, layers on independent oscillators, a contrast window that turns a field into curtains, and emitters carried by a flow field. [Aurora](docs/moonmodules/light/effects.md#aurora), [PolarNoise](docs/moonmodules/light/effects.md#polarnoise), [Tunnel](docs/moonmodules/light/effects.md#tunnel) and [Trails](docs/moonmodules/light/effects.md#trails) are written here on the published algorithms underneath (Perlin's noise, Quilez's domain warping, Bridson's curl, Stam's fluids). Stefan brought that shader vocabulary to LED panels and showed what it does there, which is the tradition the [power functions](docs/moonmodules/light/power-functions.md) and these effects sit in.
- **[hpwit](https://github.com/hpwit) (Yves Bazin)**: the clockless I2S / RMT / Parlio LED-driver techniques and the [ESPLiveScript](https://github.com/hpwit/ESPLiveScript) live-script engine behind the LED drivers and MoonLive.
- **Christophe Gagnier ([@Moustachauve](https://github.com/Moustachauve))**: author of the native [WLED-Android](https://github.com/Moustachauve/WLED-Android) and [WLED-iOS](https://github.com/Moustachauve/WLED-iOS) apps. Their source let us reverse-engineer exactly what those apps read, so projectMM devices appear in (and are controllable from) the native WLED apps.
- **The [Improv Wi-Fi](https://github.com/improv-wifi) project**: the open Improv serial provisioning standard ([sdk-cpp](https://github.com/improv-wifi/sdk-cpp) / [sdk-js](https://github.com/improv-wifi/sdk-js)) that the projectMM web installer uses to provision a freshly-flashed device over USB.
- **[FastLED](https://github.com/FastLED/FastLED)**: the canonical LED-effects library whose conventions the LED-effect world shares. projectMM links no part of FastLED, but it carries forward FastLED's recognisable *names and models* for the color/animation primitives (`scale8`, `sin8`, the gradient-palette model (`CRGBPalette16` / `colorFromPalette`), the `beatsin8` / `inoise8` / `qadd8` family), so a contributor recognises them on sight. The implementations are projectMM's own, integer-only and hot-path-tuned for our render loop; FastLED is the prior art behind the convention, credited here and in each primitive's notes.
- **[FPP](https://github.com/FalconChristmas/fpp) (Falcon Player)**: the show player that drives LED panel receiver cards from a Raspberry Pi. Seeing an FPP rig feed a wall of HUB75 panels is what prompted [PanelCardDriver](docs/moonmodules/light/drivers.md#panelcard): if a Linux host can send those frames, so can a board that is already rendering them, which removes the host from the installation entirely. FPP is the inspiration, and the reference point for what good looks like here: it sustains 50 fps.
- **[Tasmota](https://github.com/arendst/Tasmota) and Mathieu Carbou's [MycilaSafeBoot](https://github.com/mathieucarbou/MycilaSafeBoot)**: the safeboot pattern behind [MoonBase](docs/architecture.md#moonbase-the-second-boot-image): replacing a small board's second OTA slot with a minimal boot image that installs into one large app slot. Tasmota proved the scheme at scale; MycilaSafeBoot distilled it to a standalone image and set the size bar. MoonBase is our from-scratch minimal take, written directly against ESP-IDF.
- **Damian Schneider ([dedehai](https://github.com/DedeHai))**: author of the WLED Particle System, whose emitters, forces and walls over one shared pool are the shape our [particle kernel](docs/moonmodules/light/power-functions.md#particles) and the scripted `pool` / `emit` / `step` builtins follow, in our own fixed-point implementation.
- **wladi ([myhome-control](https://shop.myhome-control.de))**: designer of the [MHC-WLED ESP32-P4 shield](https://shop.myhome-control.de/en/ABC-WLED-ESP32-P4-shield/HW10027), and the source of the hardware and the pinout details that got its **line-in audio** working in [AudioService](docs/moonmodules/core/moxygen/AudioService.md): the onboard PCM1808 I2S ADC (WS 26 / SD 33 / SCK 32 / MCLK 36), the PCM1808's stereo wiring, and its `FMT` format-select jumper (open = I2S/Philips, our default; tie to 3V3 for left-justified), which is what confirmed the standard-I2S path the ADC needs.

## Contributing

projectMM is a community project, built in the open, shaped by the people who use it. We'd love to hear from you:

- **Ideas and requests**: an effect, a layout, a driver, a fixture you want supported? [Open an issue](https://github.com/MoonModules/projectMM/issues) and tell us.
- **Help build it**: pick something from the [issues](https://github.com/MoonModules/projectMM/issues), or propose a MoonModule. See [How we work](#how-we-work) for the process.
- **Test on hardware**: run it on your panels, devices, and fixtures, and report what works and what doesn't.
- **Talk to us**: questions, show-and-tell, and design discussion on [Discord](https://discord.gg/TC8NSUSCdV).

Find the MoonModules community on [Discord](https://discord.gg/TC8NSUSCdV), [Reddit](https://reddit.com/r/moonmodules), [YouTube](https://www.youtube.com/@MoonModulesLighting), and [GitHub](https://github.com/MoonModules).

## License

See [LICENSE](LICENSE).
