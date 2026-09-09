# Running projectMM on a Linux machine

projectMM runs as an ordinary Linux application: the same effect pipeline, web UI and network drivers as a board, with a real CPU behind them. A small always-on machine makes a good installation controller, whether that is a server, a Raspberry Pi, or a NanoPi.

This page is about **deploying** to such a machine. Developing on one is [building.md](../building.md), which covers the build itself and is referenced rather than repeated here.

> Windows, with screenshots: [Installing projectMM on a desktop](installing-to-desktop.md). Flashing a board: [Install & first light](../gettingstarted.md).

---

## Which route applies to your machine

The fork in the road is the CPU, so check it first:

```sh
uname -m
```

`x86_64` is an Intel or AMD machine. `aarch64` is an arm64 board: a Raspberry Pi, a NanoPi, most single-board computers.

| Your machine | Route |
|---|---|
| `x86_64` PC, server or VM | Install the released package |
| `aarch64` board (Pi, NanoPi, other SBC) | Build from source |

The released Linux binaries are x86-64 only, so an arm64 board builds from source. The recipe below
is the whole of it, and the result is the identical program.

Either route assumes a **Debian-based** system (Debian, Ubuntu, Raspberry Pi OS, Armbian), which is
what nearly every SBC image is. Another distribution works too; the package names in step 5 are the
part you would translate.

> **x64 and amd64 are the same thing**, two names for Intel/AMD 64-bit. The distinction that matters is amd64 against **arm64**, which are genuinely different machine code.

---

## x86-64: install the package

The releases page carries `projectmm_X.Y.Z_amd64.deb` for Debian, Ubuntu and Raspberry Pi OS on Intel hardware:

```sh
sudo apt install ./projectmm_X.Y.Z_amd64.deb
projectMM
```

That puts it on your `PATH`. There is also a `.tar.gz` to unpack anywhere. Both are listed in the [README](https://github.com/MoonModules/projectMM#readme).

Open `http://<machine>:8080` and the UI is there.

---

## arm64: build from source

The same recipe on a Raspberry Pi and a NanoPi, and on most other Debian-family boards. Allow an
hour the first time, most of it waiting.

### 1. Write an OS image to the SD card

**Take any Debian-based image**, and the rest of this page works unchanged. Raspberry Pi OS, Armbian
and most vendor images are all Debian underneath, so they share `apt`, the same package names, and
systemd. That is the only thing this recipe depends on, which is why it is the requirement rather
than a particular distribution.

Two ways to get one:

- **The vendor image.** For a Raspberry Pi that is [Raspberry Pi OS](https://www.raspberrypi.com/software/), and take the **Lite** build: the desktop one carries a lot a controller never uses, and leaves less memory for the build. For a NanoPi it is the FriendlyELEC image linked from that board's wiki page.
- **[Armbian](https://www.armbian.com/)**, which is the better answer as soon as you have more than one kind of board: Raspberry Pi OS is for the Pi alone, while Armbian covers Rockchip and Amlogic boards too, from one project and with more regular updates than most vendor images. Check its [board list](https://www.armbian.com/download/) first, since coverage varies and a board can be in development without a released image.

**What to avoid is a router firmware.** FriendlyELEC ships **FriendlyWrt** (OpenWrt-based) alongside
Debian, Ubuntu and Buildroot for the NanoPi R28S, all from [their wiki](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_R28S#Flashing_the_OS_to_the_microSD_card).
OpenWrt uses a different package manager and none of the steps below apply to it. Prefer a newer
Debian release where the vendor offers one, for the longer-supported kernel.

Write it with [Raspberry Pi Imager](https://www.raspberrypi.com/software/) or [balenaEtcher](https://etcher.balena.io/), both of which take the compressed download directly.

**On a Raspberry Pi, use Imager's settings dialog before writing.** Current Raspberry Pi OS ships with no default user and **SSH switched off**, so a card written without it boots to a machine you cannot log in to remotely. The dialog sets the username and password, the hostname, your WiFi credentials, and enables SSH. Doing it here saves needing a keyboard and monitor later.

### 2. First boot

Put the card in, plug in the network cable, then power. **Give it 10 to 20 minutes**: a first boot resizes the filesystem and generates host keys, and the board may reboot itself while doing so. It is not stuck.

Then find it on the network. Any of these works:

```sh
ping raspberrypi.local          # or NanoPi-R28S.local
arp -a                          # everything the network has seen
```

Your router's client list is the reliable fallback when mDNS is not resolving.

### 3. Log in

```sh
ssh pi@NanoPi-R28S              # FriendlyELEC Debian: user pi, password pi
ssh <you>@<hostname>.local      # Raspberry Pi OS: the user you set in Imager
```

On the FriendlyELEC Debian image the hostname is the hardware model, so `NanoPi-R28S` resolves without a `.local` suffix, and the root account is disabled (`sudo passwd root` if you ever want it). Credentials differ per image and the board's own wiki is the authority. **Change a default password immediately:**

```sh
passwd
```

If it is not on the network yet, attach a keyboard and monitor, or a USB serial adapter, and configure it there:

```sh
sudo nmtui                      # a menu for WiFi and static addresses
ip ad                           # what addresses the board actually has
```

### 4. Bring the system up to date

```sh
sudo apt update
sudo apt upgrade -y
```

On a fresh image this can take a while. Worth doing before building, so the compiler and libraries you build against are the ones you keep.

### 5. Install the prerequisites

```sh
sudo apt install -y python3-pip cmake build-essential git
pip install uv --break-system-packages
```

`--break-system-packages` looks alarming and is routine: Debian 12 and later mark the system Python
as externally managed, and this flag is how a user-level tool installs anyway. It affects pip's own
environment, not the system.

If `uv` is not found afterwards, it landed in `~/.local/bin`, which is not always on the path:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

### 6. Build and run

```sh
git clone https://github.com/MoonModules/projectMM.git
cd projectMM
uv run moondeck/build/build_desktop.py
uv run moondeck/run/run_desktop.py
```

The build takes a few minutes on a Pi 4 or a NanoPi, longer on older boards. Everything about the
build itself, including how to run the tests, is in [building.md](../building.md).

`run_desktop.py` detaches, so the program outlives the ssh session. Open `http://<board>:8080` and
the UI is there.

**That is a running system.** The rest of this page makes it survive a reboot and covers the two
boards; none of it is needed to start building shows.

> **A board with 1 GB of RAM or less can run out of memory while compiling.** The symptom is the
> compiler being killed rather than an error you can read. Either add swap
> (`sudo dphys-swapfile swapoff && sudo nano /etc/dphys-swapfile` to raise `CONF_SWAPSIZE`, then
> `swapon`), or build with fewer parallel jobs.

## Keeping it running after a reboot

So far projectMM stops when the board restarts. A permanent installation wants it back by itself. Give it a systemd unit at `/etc/systemd/system/projectmm.service`:

```ini
[Unit]
Description=projectMM
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/home/pi/projectMM/build/projectMM
Restart=always
RestartSec=5
User=pi

[Install]
WantedBy=multi-user.target
```

Then:

```sh
sudo systemctl enable --now projectmm
systemctl status projectmm
```

`Restart=always` covers a crash as well as a reboot. Adjust `User` and the path to match where you built it.

## Shutting down

**Do not pull the power.** An SD card interrupted mid-write can corrupt the filesystem, and the board then does not come back:

```sh
sudo shutdown now     # or: sudo reboot
```

projectMM itself writes to disk rarely (settings on change, not per frame), so an SD card is a fine home for it. The risk is the operating system's writes, not ours.

---

## Board notes

### Raspberry Pi

A Pi 4 or 5 has ample headroom for the render pipeline. Prefer the **Lite** image: a controller has
no use for a desktop, and it leaves more memory for the build.

### NanoPi R28S

A small metal-cased board with **two Gigabit ethernet ports**, so it can sit between a house network
and a lighting network. The case is the heatsink, so it runs without a fan. It boots from SD; no
eMMC needed.

- **Images**: FriendlyELEC offers FriendlyWrt, Debian, Ubuntu and Buildroot. **Take Debian**: FriendlyWrt is router firmware, with a different package manager and none of this recipe. Armbian has RK3528 work in progress but publishes no R28S image at the time of writing.
- **Finding the image** is the fiddly part. The [wiki's flashing section](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_R28S#Flashing_the_OS_to_the_microSD_card) links a Google Drive; the file you want is under **`01_Official images/01_SD card images`**, a `.gz` you can hand to Imager or Etcher without extracting. The other directories are for installing to eMMC, which you do not need. Use an **8 GB card or larger**.
- The wiki's own instructions assume Windows and `win32diskimager`. Imager or Etcher do the same job on macOS and Linux.
- **It has 1 GB of RAM**, which is enough to run projectMM comfortably and tight for compiling it. If the build is killed, add swap as described in step 6.
- **Configure the second port** with `sudo nmtui`; `ip ad` shows what each picked up.

---

## Containers

A container image is [in development](https://github.com/MoonModules/projectMM/pull/98), not yet
released. When it lands it will run a full instance on anything that runs Docker.

One thing to know before reaching for it on a board: **a container does not emulate a CPU.** It shares the host kernel and runs native instructions, so an amd64 image needs an amd64 host. Docker Desktop on Apple Silicon is the exception, bundling emulation so an amd64 image runs (slowly) on an arm64 Mac. An arm64 SBC has no such emulator, so on a Pi or a NanoPi, build from source as above.

---

## Where to go next

- [Install & first light](../gettingstarted.md): the same program on an ESP32.
- [How projectMM works](how-projectmm-works.md): layouts, layers, effects and drivers.
- [building.md](../building.md): building, testing and packaging in depth.
