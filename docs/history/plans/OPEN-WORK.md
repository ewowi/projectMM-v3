# Open work in the unarchived plans

What is left in each plan still sitting in this folder, audited against the tree on 2026-09-07. One
line per item, pointing rather than restating: the plan itself is the description, this is the
worklist. A plan leaves this file and moves to `archive/` when its last line here is struck.

Everything not listed here shipped and was archived.

## Small, and closable in a sitting

| plan | what is left |
|---|---|
| **MoonLive palettes** | The promised scenario: a scripted palette driving a real effect end to end. Everything else landed. |
| **OSC control ingest** | `unit_OscModule` and an OSC scenario, both promised. Only `unit_OscPacket` exists. |
| **Two-way control surfaces** | `unit_ControlSurface` + `unit_OscModule`, a scenario, and the seam section in `docs/reference/control-surfaces.md`. The code is complete. |
| **Config backup and restore** | One sentence: the installer's erase-confirm should point at backing up config first (`mooninstaller/install.js`). |

## Bench-gated: needs hardware, not keyboard time

| plan | what is left |
|---|---|
| **Input mapping and scripted sensors** | Steps 2 and 3 are host-verified but NOT bench-verified. Also open: the `.mls` picker confirmation and the `services.md` MoonLiveService card. |

## Larger, and its own effort

| plan | what is left |
|---|---|
| **Input mapping and scripted sensors** | Steps 4 / 4b / 5 / 6 not started: I2C sensors (MPU6050), VL53L8CX zone grid, pulse timing + `EncoderService`, PIR level events. |
| **MoonLight migration (multi-stage)** | Stage 5's transport (wired DMX-512 in/out) is the one Must-class item for the rename. Also 3 installation-specific layouts, ~31 effects (mostly few-line ports), and LightsControl. Its own Status section has the detail. |

## Worth knowing

**Desktop audio capture is shipped** and archived: its text still says "remaining before shipped",
but the fleet test it names was later marked verified, leaving only a post-merge run by a Windows
tester, which is not a deliverable.

**Four of the six partials are missing only tests or a doc line.** That is the pattern worth acting
on: the features landed and the pinning did not, which is exactly the gap that goes unnoticed until
something breaks. Closing all four is a sitting's work and would archive three more plans.
