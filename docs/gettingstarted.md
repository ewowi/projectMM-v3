# Getting started

New to ESP32 or flashing firmware? You don't need to be. projectMM installs
straight from your web browser — no software to download, no command line. In a
few minutes you'll have lights running and the device on your network, and the
device's own web interface open in your browser ready to play with.

This guide has two chapters. **Chapter 1** gets projectMM onto your device.
**Chapter 2** is a tour of the interface you land in afterwards, so you know what
every part does and where to start building your own light show.

**You need:** an ESP32 board, a USB cable that carries data (not charge-only),
and a **Chromium-based browser** on a computer — Google Chrome, Microsoft Edge,
or Opera (the installer uses the Web Serial API, which Safari and Firefox don't
support).

> Want the bigger picture of what projectMM is first? See the
> [project overview](../README.md).

---

## Chapter 1 — Install projectMM

### 1. Open the installer and plug in

Open the **[web installer](https://moonmodules.org/projectMM/install/)** in
Chrome or Edge, then plug your ESP32 into a USB port.

![The web installer](assets/gettingstarted/01-01-installer-start.png)

### 2. Pick the USB port

Click **USB Port → Pick a port…**. Your browser shows a small list of connected
devices — choose the one that appeared when you plugged in the ESP32. (Not sure
which? Unplug, look at the list, plug back in — the new entry is your device.)

![Selecting the USB port](assets/gettingstarted/01-02-select-port.png)

**Windows users — dialog says "No serial ports available"?** Windows doesn't
ship drivers for the USB-serial chips most ESP32 boards use (WCH CH340, Silicon
Labs CP2102). One-time install fixes it for every future flash — full
step-by-step + the download link is in
[building.md § Windows: USB-serial drivers](building.md#windows-usb-serial-drivers).
macOS and Linux ship these drivers built in, so it's a Windows-only step.

Once a port is chosen, the installer recognises the chip and tells you how many
devices match it, so you know you're on the right track before you pick one.

![Port selected, chip detected](assets/gettingstarted/01-03-port-selected.png)

### 3. Pick your device

Choose your device from the **Device** picker. Each card shows a picture, the
chip, and what the device can do (LEDs, WiFi, a button, a microphone…); click
**details** on any card to see exactly what it is and a link to its product page.

![Picking a device](assets/gettingstarted/01-04-pick-device.png)

![A device card with its details](assets/gettingstarted/01-05-device-details.png)

The little colored pills are the device's capabilities, and the color tells you
how ready each one is:

- 🟢 **Green** — set up and working the moment you install. This capability is
  supported *and* already wired into the device's configuration.
- 🟡 **Yellow** — the firmware supports it, but it isn't pre-configured. It works
  once you add and set up the matching module yourself in the UI (Chapter 2).
- 🟠 **Amber** — planned. The hardware has it, but there's no module for it yet —
  it's on the to-do list. (Want to help? Building one is our usual loop: read the
  product page and datasheet, pin the behaviour as tests, then write the code to
  pass them — [see how we work](../CLAUDE.md#principles).)

So a green pill is "just works", a yellow one is "works, with a bit of setup", and
an amber one is "coming later".

The setup panel then shows how your device is configured out of the box — the
modules and settings applied automatically when you install.

![Device setup](assets/gettingstarted/01-06-device-setup.png)

Nothing is locked in: once the device is running you can change any of it later
in the UI (that's what Chapter 2 is all about).

Leave **Release** and **Firmware** at their suggested values (the newest stable
build, and the firmware that matches your device). Tick **Erase chip first** only
if you're starting clean, switching firmware, or updating a 4 MB classic board
(esp32 / wrover / eth) from a release before v4.0. That last update must erase:
its partition layout changed ([MIGRATING](MIGRATING.md)), and if the device already holds
config you care about, back it up first ("Back up a device's config first" on the
installer page): erasing wipes WiFi credentials and all settings, and the backup
brings them back after the flash (its report lists anything it could not carry).

### 4. Click Install

The installer erases (if you asked it to) and writes the firmware. Just watch —
it takes under a minute.

![Erasing](assets/gettingstarted/01-07-erasing.png)
![Installing](assets/gettingstarted/01-08-installing.png)

### 5. Get it on your network

What happens next depends on your device:

- **WiFi:** enter your network name and password when prompted, then **Connect**.
  (Click **Skip** to set WiFi up later from the device itself.) Restoring a config
  backup? You can skip this step: join the device's `MM-XXXX` access point, open
  `http://4.3.2.1`, and restore the backup in the File Manager (⟲), then take the
  offered restart: the bundle carries the WiFi credentials, so the device joins
  your network by itself.

  ![Entering WiFi credentials](assets/gettingstarted/01-09-wifi-credentials.png)

- **Ethernet:** plug in the cable — it connects on its own, no password needed.

### 6. Open your device

When it's online, the installer shows a link — your device's address on your
network. Click it.

![Device is online over WiFi](assets/gettingstarted/01-10-online-wifi.png)

You'll see this same "Device is online!" box however your device connected — over
Ethernet, or when it rejoins a network it already knows:

![Online over Ethernet](assets/gettingstarted/01-11-online-ethernet.png)
![Online on an address it already had](assets/gettingstarted/01-12-online-existing-ip.png)

That's it — projectMM is installed and on your network. The link opens the
device's own web interface, served straight from the ESP32. Let's look around.

---

## Chapter 2 — Your projectMM interface

Everything below runs **in your browser, live from the device**. There's no app,
no account, no cloud — the ESP32 itself serves this page, and every change you
make takes effect on the lights immediately. Open the link from step 6 and follow
along; you can't break anything by exploring.

### The layout: list, preview, controls

![The full projectMM interface](assets/gettingstarted/02-01-UI-large.png)

Three regions, left to right:

- **The module list** (left) — every part of your device, from system info at the
  top to your light setup at the bottom. Click a name to jump to it.
- **The 3D preview** (centre) — a live picture of your lights in their real shape,
  updating as the effects run. This is what your physical LEDs are doing, right now.
- **The controls** (right) — the settings for each module. Drag a slider or pick an
  option and the lights react instantly.

Every module header carries a **⏻ power button** — it turns that module on or off.
Bright (accent-colored) means on; dimmed means off. A switched-off module simply
stops running — it stays in place with all its settings, so flicking it back on
picks up right where it left off. It's the quick way to mute an effect or an output
for a moment without deleting anything.

You'll also spot two little read-outs in each header: **🕒** is how fast that module
runs (its loop speed — click it to flip between fps and microseconds), and **🧠** is
how much memory it uses. They let you see at a glance what each part is costing.

The interface adapts to your window. On a narrower screen the controls take the
full width and the preview tucks into a floating thumbnail you can move around:

![Medium width — preview as a floating thumbnail](assets/gettingstarted/02-02-UI-mid.png)

Narrower still, it stacks into a single scrollable column — so it works on a
phone, standing next to your lights:

![Small width — single column](assets/gettingstarted/02-03-UI-small.png)

### The 3D preview

![The 3D preview, lights numbered](assets/gettingstarted/02-04-UI-Preview.png)

Drag to rotate, scroll to zoom. Each dot is one light at its real position, lit
with the color it's showing this instant. Turn on the numbers to see each light's
index — handy when you're wiring or mapping a layout. The preview is a *view* of
the device; it never slows the lights down, and it gracefully eases off (fewer
updates, then fewer points) on a slow connection rather than stalling.

> More on how the preview streams from the device:
> [PreviewDriver](moonmodules/light/moxygen/PreviewDriver.md).

### The system modules

The top of the list is your device's "about" section — read-outs and connection
settings. You rarely need to touch these, but they're the first place to look if
something seems off.

**System** — who this device is and how it's doing: its name, the device model,
uptime, frame rate, and live memory / storage bars. You may also see an **Audio**
module here — devices with a built-in mic come with it set up for you, and on any
device you can add it yourself (it's how audio-reactive effects hear the music).
Audio is just the first of many: any sensor or input — from hardware or over the
network — lives here as its own module, and we're adding more all the time.

![The System module](assets/gettingstarted/02-05-UI-System.png)

> [SystemModule](moonmodules/core/system.md#system) ·
> [Audio](moonmodules/core/services.md#audio)

**Firmware** — which build you're running, and where you update it. The
**Install** button here does an over-the-air update straight from the device — no
USB cable needed once it's on your network.

![The Firmware module](assets/gettingstarted/02-06-UI-Firmware.png)

**Updating from an older build?** Skim the [migration notes](MIGRATING.md) first. Most updates need nothing — the device keeps your settings — but a breaking change is listed there with the one action it costs you (usually re-setting or re-adding a control).

> [FirmwareUpdateModule](moonmodules/core/system.md#firmware-update)

**Network** — your connection: WiFi or Ethernet, signal strength, and the
address others reach it at. The **Devices** section underneath finds other
projectMM devices on the same network, so a roomful of them can discover each
other.

![The Network module](assets/gettingstarted/02-07-UI-Network.png)

> [NetworkModule](moonmodules/core/system.md#network) ·
> [DevicesModule](moonmodules/core/system.md#devices)

> **Lights are just one use.** Everything above — the modules, the live controls, the
> 3D view, the web UI, the networking — is a general-purpose engine that knows nothing
> about LEDs. The light show below is one *domain* built on top of it; you could build
> a different one and reuse all the same machinery. [FastLED-MM](https://github.com/MoonModules/FastLED-MM)
> is an example, driving its LEDs with [FastLED](https://github.com/FastLED/FastLED) (on
> hold until projectMM ships as a reusable library).

### Control it from your phone with WLED Native

The device's own web UI works on a phone, but for quick on/off and brightness from
your pocket there's a nicer option: **WLED Native**, the open-source mobile app for
the WLED ecosystem. projectMM speaks the WLED JSON API and announces itself over the
network the same way a WLED device does, so the app finds your projectMM controllers
automatically — no setup, no pairing. Each one shows up as a card with a power toggle
and a brightness slider, so a roomful of controllers is a scroll and a tap away.

![projectMM devices discovered in WLED Native](assets/core/WLED%20Native%20discovers%20projectMM.jpeg){ width="300" }

Get it free for your phone:

- **iPhone / iPad:** [WLED Native on the App Store](https://apps.apple.com/us/app/wled-native/id6446207239)
- **Android:** [WLED Native on Google Play](https://play.google.com/store/apps/details?id=ca.cgagnier.wlednativeandroid)

WLED Native is by **Christophe Gagnier ([@Moustachauve](https://github.com/Moustachauve))**, who wrote both the [Android](https://github.com/Moustachauve/WLED-Android) and [iOS](https://github.com/Moustachauve/WLED-iOS) apps. Their open source is what let us work out exactly what those apps read, so a projectMM device appears in them without either side needing to know about the other.

For the full picture and controls, the device's web interface is always there at
`http://<devicename>.local` — WLED Native is the fast everyday remote alongside it.

### Bring it into your smart home with Home Assistant

Want your lights in the same dashboard as the rest of your house — and in
automations, voice assistants, and Apple Home? projectMM adopts into **Home
Assistant** like any other light: point the device at your HA setup and it appears
as a light entity with on/off and brightness, alongside a floor of other devices.

![projectMM devices as lights in a Home Assistant dashboard](assets/core/ha-integration.png){ width="600" }

There are two ways in — zeroconf (HA finds the device on its own) or MQTT
auto-discovery (for a broker-only or cross-subnet setup) — and from there you can
bridge the entity into Apple Home too. The step-by-step, including installing HA
and the MQTT broker if you don't have them, is in the
[home automation guide](usecases/home-automation.md).

### Building a light show: layouts → layers → drivers

The bottom three modules are where the fun is. They form a simple pipeline: a
**layout** says where your lights are, **layers** decide what colors play on
them, and **drivers** send the result out to the real world. Add modules with the
dashed **+ add module** button under each one.

**Layouts** — the shape of your lights. The default **Grid** is a width × height
(× depth) of pixels; change the numbers and the preview reshapes instantly. Turn
on **serpentine** if your strip zig-zags back and forth.

![The Layouts module](assets/gettingstarted/02-08-UI-Layouts.png)

> [Layouts](moonmodules/light/supporting.md)

**Effects** — what plays on the lights. Add an **effect** (a moving pattern), stack
several to blend them, and reshape them with **modifiers** (mirror, rotate, and
more). Each effect has its own controls — speed, color mode, and so on — that you
tweak live.

![The Effects module](assets/gettingstarted/02-09-UI-Layers.png)

> [Effects](moonmodules/light/supporting.md) · [Layer](moonmodules/light/supporting.md)

**Drivers** — where the colors go. Set overall **brightness** and color order,
then add an output: real LED strips on a pin, or send the frame over the network
(ArtNet, E1.31/sACN, DDP) to other devices or lighting software.

![The Drivers module](assets/gettingstarted/02-10-UI-Drivers.png)

> [Drivers](moonmodules/light/supporting.md) ·
> [NetworkSendDriver](moonmodules/light/moxygen/NetworkSendDriver.md)

That's the whole picture: **layout → layers → drivers**, previewed in 3D, all
tuned live in your browser. Pick an effect, drag a slider, watch the lights — then
keep going.

---

### Where to go next

- **Understand the pipeline** — how layouts, layers, effects, modifiers and
  drivers fit together: [architecture overview](architecture.md#the-pipeline).
- **Run it on your computer** instead of (or alongside) an ESP32 — macOS, Windows,
  Linux: [project overview → Getting started](../README.md#getting-started).
- **Manage several devices, build, and flash from one console** with MoonDeck, our
  developer tool: [MoonDeck guide](../moondeck/MoonDeck.md).
- **Build from source** or target Teensy / Raspberry Pi: [building.md](building.md).

Stuck, or something didn't work? Open an
[issue](https://github.com/MoonModules/projectMM/issues) — and tell us what device
you used and where it stopped.
