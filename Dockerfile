# projectMM as a container: the desktop firmware, which is the whole system without an ESP32.
#
# The desktop build is not a simulator. It runs the same effect pipeline, the same web UI and the
# same driver stack as a board, and it drives real fixtures over Art-Net, DDP and E1.31, so a
# container is a complete live installation for anyone whose fixtures are on the network.
#
# It INSTALLS the released .deb rather than building from source, deliberately. The release already
# produces that package; building here would be a second build path to keep working, and the two
# would drift. The image is packaging, not a build.
#
#   docker build -t projectmm .                              # the rolling prerelease (default)
#   docker build --build-arg RELEASE=stable -t projectmm .   # the newest stable release
#   docker build --build-arg RELEASE=v4.0.0 -t projectmm .   # a specific tag
#   docker run --rm -p 8080:8080 -v projectmm:/data projectmm
#
# Then open http://localhost:8080/.
#
# **Ports.** 8080 is the web UI, and the only port needed to try it out. Driving fixtures is
# OUTBOUND: Art-Net on UDP 6454, DDP on 4048, E1.31/sACN on 5568. Those are L3 and reach a unicast
# fixture address through ordinary bridge networking.
#
# **When L2 matters.** mDNS discovery (finding boards, being found by them) is multicast and does
# not cross a bridge network, and Art-Net's broadcast mode has the same problem. For those, attach
# the container to the host's network directly (`--network host`, or an L2 CNI on Kubernetes).
# Unicast output needs none of it. NOT verified on a Linux host yet: on macOS and Windows, Docker
# Desktop runs a Linux VM, so `--network host` joins the VM rather than the machine's LAN and the
# question cannot be answered there.
#
# **Capabilities.** None. It binds 8080 as an ordinary process and needs no added capability.
#
# **amd64 only.** The release ships no arm64 LINUX binary (macOS arm64 is a different target), so
# an arm64 image needs an arm64 build in the release pipeline first, not a change here.

# --- stage 1: fetch the release and unpack it -------------------------------------------------
# A full Debian image, used only to resolve and extract the .deb. None of it reaches the result.
FROM debian:trixie-slim AS fetch

# WHICH release to install, and the default is the ROLLING PRERELEASE, matching what the installer
# page offers rather than the last tagged version: projectMM ships from `main` continuously, so a
# tagged release can be months behind what a board would be flashed with, and an image that lagged
# the firmware would be the wrong thing to test against.
#
# `latest` here is a real git TAG carrying that rolling build, not GitHub's "latest release" idea.
# `stable` is the special value asking for GitHub's newest NON-prerelease, and anything else is
# taken as a literal tag. The two words genuinely differ, which is why both exist.
ARG RELEASE=latest
ARG REPO=MoonModules/projectMM

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && if [ "$RELEASE" = "stable" ]; then \
        api="https://api.github.com/repos/${REPO}/releases/latest"; \
    else \
        api="https://api.github.com/repos/${REPO}/releases/tags/${RELEASE}"; \
    fi \
 && url=$(curl -fsSL "$api" | grep -o 'https://[^"]*_amd64\.deb' | head -1) \
 && test -n "$url" || { echo "no amd64 .deb in release ${RELEASE}" >&2; exit 1; } \
 && curl -fsSL -o /tmp/projectmm.deb "$url" \
 && dpkg-deb -x /tmp/projectmm.deb /rootfs

# --- stage 2: the image that ships ------------------------------------------------------------
# Distroless: the binary plus its four shared libraries, with no shell and no package manager, so
# the attack surface is the application rather than a distribution. `ldd` on the release binary
# lists exactly libstdc++, libm, libgcc_s and libc, which is the whole reason this fits: nothing
# else has to come along. 45 MB against 140 MB for the full-Debian form.
#
# **debian13, NOT debian12**, and this is load-bearing. The release is built on ubuntu-24.04
# (glibc 2.39), so the binary requires glibc >= 2.38. The debian12/bookworm images ship 2.36, where
# it installs cleanly and then dies at startup with "GLIBC_2.38 not found" from libc and libm.
# Verified both ways on the bench. If the release ever moves to an older builder, this can too.
FROM gcr.io/distroless/cc-debian13

COPY --from=fetch /rootfs/usr/bin/projectMM /usr/bin/projectMM

# WHERE THE CONFIG LIVES, and why this line is required rather than a convenience. The desktop
# build resolves its data directory from the environment (platform_desktop.cpp, userDataDir): on
# Linux XDG_DATA_HOME first, then HOME/.local/share. A container has NEITHER, and the function then
# returns empty, so without this the app has nowhere defined to write. Setting it explicitly also
# gives the volume one documented path instead of a guess: config lands in /data/projectMM/.config.
ENV XDG_DATA_HOME=/data
VOLUME /data

EXPOSE 8080

ENTRYPOINT ["/usr/bin/projectMM"]
