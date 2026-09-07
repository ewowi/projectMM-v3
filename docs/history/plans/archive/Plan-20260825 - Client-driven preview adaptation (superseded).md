# Plan, Client-driven preview adaptation

(Superseded by the Lean preview transport plan: the fps-band controller and the push-model transport it steered were replaced by the pull model with the drops signal.)

## Context: a day of device-side guessing

The preview got its own lossy channel (`/wsp`), a subscriber gate, backpressure and a
per-message frame drop, all sound, all staying. On top of that, the device grew an
adaptation stack that guesses link quality from how fast its socket drains: first
frame-counted streaks (oscillated, 49 rebuilds in 25 s on WiFi), then a wall-clock
window controller with three bands, a starvation detector, a failed-stride memory with
decay, and an adaptive point ceiling with earn/give-back.

Each fix compensated for the last, and the result still cycles: the ceiling has no
failure memory, so a link that carries 8,192 points but not 16,384 re-earns the higher
ceiling every ~10 good seconds, fails, gives it back, and repeats, bench-confirmed as
"runs okay a few seconds, throttles, sometimes recovers", on a single client with no
network contention. **This is a regression against the pre-controller state**, which the
product owner had judged good.

The structural error, named by the product owner: **the device is the wrong party to
measure**. It sees only its socket. The browser knows everything that matters, bytes
received, frames rendered, render cost, the `targetFps` the user chose, and whether the
tab is even visible.

## The decision: the receiver adapts, the device serves

This is the industry-standard shape for exactly this problem. Adaptive video streaming
(HLS/DASH) settled it years ago: the **receiver** measures its own throughput and
requests a quality level; the server serves what is asked and guesses nothing.
Client-side adaptation is the standard *because* the receiver is the only party that can
measure end-to-end.

Applied here:

- The **browser** runs the one controller, in one place, in debuggable JavaScript: it
  measures its achieved frame rate against `targetFps` and requests a stride over the
  `/wsp` socket it already holds.
- The **device** serves the requested stride and keeps only true floors: the fps send
  gate, drop-frame-when-buffer-full (the probe drop), the memory cap, and a **fixed**
  display cap. It deletes the entire guessing apparatus.

What this buys beyond the deletion: no coupled oscillations (each client tunes only
itself), a slow laptop finally gets relief even on a fast link (the robwomp gap, render
cost is now part of the measurement), tab visibility integrates naturally, and adaptation
policy ships with the served UI instead of requiring firmware flashes to tune.

## Design

### 1. The uplink message (client → device, on `/wsp`)

The channel is currently downstream-only. The client gains one message:

    [0x51][stride u8]        "serve me every stride-th light"

Browser WS frames are always masked (RFC 6455); the device unmasks exactly as
`pollWledStateFromWebSockets` already does for the WLED shim. Parsed in the existing
per-tick client poll, off the hot path. Unknown opcodes stay ignored.

**Multiple clients, v1 rule: the device serves the COARSEST requested stride to all.**
One lattice, one coordinate table, one frame per slot, no per-client render work. A slow
viewer coarsens the shared preview; accepted for v1 (the realistic case is one viewer,
occasionally two) and recorded here as the known trade. Recomputed when a request arrives
and when a client disconnects.

### 2. The client controller (`preview3d.js`)

A pure function, so it is unit-testable in `test/js` without a DOM:

    nextStrideState(state, achievedFps, targetFps)
      achieved < 20%                       → coarser immediately (true starvation)
      achieved < 60% for 2 windows (4 s)   → coarser (double, cap 64); remember current as failed.
                                             Two windows, so a single GC pause never costs a
                                             visible rebuild (bench: one bad window coarsened)
      achieved ≥ 80% for 3 windows (6 s)   → finer (halve), unless halving lands on failedStride
                                             (skip once, then clear, the decay rule, kept from
                                             the device version because it worked). 80, NOT 95:
                                             sender-slot quantisation and jitter keep a perfect
                                             link under ~95%, and a bar the link cannot clear
                                             strands the preview coarse until a refresh (bench)
      otherwise                            → hold

Driven every 2 s from the frames the client already counts to render. `targetFps` is read
from the state the UI already has. The status line ("preview 1/N · X fps") keeps working,
the client now *owns* those numbers instead of decoding them.

### 3. Tab visibility (product-owner ask): a hidden tab costs the device nothing

`document.visibilitychange` drives BOTH sockets, on two clocks:

- **`/wsp` closes immediately** on hide, it is the expensive stream, and the driver then
  builds no frames at all.
- **`/ws` closes after a ~10 s grace**, the 1 s state pushes (and their per-second tree
  serialization) stop too, so the device is left doing nothing but rendering effects. The
  grace means a quick alt-tab does not pay a full-state resync on every switch; the push
  loop already short-circuits with no clients, so no firmware change is needed for the
  saving itself.

On return: reopen `/ws` first (the full-state resync repaints the UI), then `/wsp` with
the last requested stride. This also eliminates the worst client shape on BOTH channels,
a backgrounded tab whose throttled JS drains at a trickle while staying subscribed.

### 4. The device simplification (the point)

**Deleted from `PreviewDriver`:** the window controller (`winStartMs_`, `winFrames_`,
`winSlots_`, `goodWindows_`, `kWindowMs`, `kMinSlots`, `kRefineWindows`, the three bands,
the starvation branch), the adaptive ceiling (`displayCap_` earn/give-back), the failure
memory (`failedStride_`, `lastTarget_`), roughly 150 lines and 8 members, plus their
unit tests. `downscale_` becomes the requested stride (link factor), still combined with
the memory/display cap exactly as today.

**Kept:** the fps gate (`targetFps` remains the ceiling the device never exceeds), the
lattice + closed-form dense counting + `keptIdx_` cache, `coordPending_` retry, the probe
drop and time-budget send in `HttpServerModule`, the reap, `MAX_PREVIEW_CLIENTS`.

**Added:** `clients: N` and the served stride in the driver status, the observability the
bench work had to reconstruct with four-probe tricks.

## Steps

1. **Device: uplink parse + coarsest-of-clients + status.** Unit tests: a masked 0x51
   frame sets the stride; two clients → coarsest wins; disconnect recomputes; unknown
   opcode ignored; status carries `clients`/stride.
2. **Device: delete the controller.** Remove the members, bands and their tests; pin the
   new contract with one test: the stride never changes without a request or a rebuild.
3. **Client: `nextStride` + the 2 s loop + request sender.** `test/js` pins the bands,
   the failed-stride skip-once-then-clear, and that hidden tabs send nothing.
4. **Client: visibilitychange → both sockets.** Preview closes on hide, control after the
   ~10 s grace; return reopens control-then-preview. JS tests pin the wiring, the grace
   (a quick hide/show never drops `/ws`), and that a hidden tab sends nothing.
5. **Docs**: driver card (targetFps wording: the client aims for it), architecture § the
   channel gains its uplink sentence, MIGRATING note (behavioral, no key changes).
6. **Bench, the regression bar:** all four boards + desktop at 128×128 and 64×64. Success
   = settles within ~6 s of a slider move, no cycling over 5 minutes, tab-hide drops
   device work to zero (preview instantly, state pushes after the grace, verify with the
   connection count and the render fps rising), S3/WiFi shows the coarse-but-smooth trade at high targets and
   full-detail-slow at low targets.

## Risks

1. **A hostile/buggy uplink**, parsed bytes from the network: bounds-check stride to
   [1, 64]; anything else is ignored. The parser is ~10 lines beside an existing one.
2. **Coarsest-of-clients** lets one throttled viewer degrade the shared preview, v1
   trade, documented; per-client lattices are the later fix if it ever bites.
3. **The client controller can be wrong too**, but it is one pure function with unit
   tests, hot-reloadable with the page, and it measures the true end-to-end quantity.
4. **Old UI against new firmware** (or reverse): a client that never sends 0x51 gets
   stride 1 capped by memory/display caps, the pre-adaptation behavior; a new client
   against old firmware sends an opcode the device ignores. Both degrade soft.

## Verification

`ctest` + `test/js` + scenarios + spec check; the bench matrix in step 6; and the product
owner's eyes on the S3/WiFi case that has been today's truth-teller.
