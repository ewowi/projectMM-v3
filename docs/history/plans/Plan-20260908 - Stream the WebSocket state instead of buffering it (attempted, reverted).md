# Plan: stream the WebSocket state instead of buffering it whole

> **Reverted the same day.** The design was built and verified on the bench (the Dig-Octa received
> its 44,539-byte state as 6 fragments, byte-identical to `/api/state`, where it had been truncating
> at 30,719), and then backed out: the chain still holds the WHOLE document, as slices, for the
> duration of the drain. On a 65 KB board a UI refresh (a second `/ws` client mid-drain, the `/wsp`
> preview, plus `/api/types` and `/api/state` fetches) took the heap under what lwIP needs and the
> board crashed. The bench check had been one raw client on one socket, which proved the framing and
> not the memory budget.
>
> The product owner's call was to reconsider the architecture instead: **snapshot over HTTP, deltas
> over WebSocket**. `GET /api/state` already streams through a 1 KB socket-mode sink with no document
> in RAM, and the value patches already exist; what is missing is the UI fetching the snapshot on WS
> open and a small `{"resync":true}` on a structural change. That deletes the full-state-over-WS path
> rather than shrinking it. Tracked in [backlog-core.md](../../backlog/backlog-core.md).
>
> Kept from this work: `JsonSink` now FLAGS a refused heap grow instead of truncating silently
> (`unit_JsonSink_overflow`), which is the bug that made a cut document indistinguishable from a
> whole one.

## The problem, measured

A classic ESP32 cannot build its own state document. `buildStateJson` serializes the whole module
tree into one heap buffer, and on the bench (2026-09-08) that document is **44,261 bytes** on the
QuinLED Dig-Octa and **42,365** on the Olimex. The buffer grows by doubling, and old and new are both
live across the `memcpy`, so reaching 64 KB asks for roughly 96 KB of CONTIGUOUS heap. The Dig-Octa
has 57 KB as its largest block.

The failure was silent and total. `JsonSink::append` dropped on a refused allocation and returned
without a flag, `buildStateJson` finished "successfully" holding a truncated document, and
`startBufferedTextSend` wrote a correct WebSocket header declaring it complete. The browser parsed
it, threw, and dropped every module past the cut: **effect cards simply vanished**, with nothing
logged on the device or in the console. Both classic boards shipped a cut document, the Dig-Octa
losing 11,226 bytes and the Olimex 8,101.

Two fixes already landed and neither is sufficient:

- `JsonSink::append` now sets `overflowed_`, so a truncated document is detectable.
- `ensureHeap` backs off in quarters when a doubling is refused, so growth lands on a size the heap
  can serve rather than giving up. That moved the Dig-Octa's cut from 16,383 to 30,719 bytes. It
  still does not reach 44,261, and no back-off can: grow-and-copy needs old plus new at once.

One dead end is worth recording. Refusing to send an overflowed document (returning early) FROZE THE
WHOLE UI: `fullResyncPending_` stayed set, the else-branch that pushes value patches was never
reached, and the header stopped updating fps and free heap. A partial tree that keeps updating beats
a whole one that never arrives. That refusal was replaced by a warning.

**Ethernet dissolves it and is not the answer.** A board on Ethernet never starts the WiFi stack and
keeps ~50 KB more heap, which is why the Olimex (Ethernet) looks healthy and the Dig-Octa (WiFi STA)
does not. Most users are on WiFi, so the fix has to work there.

## What the current design buys, and must keep

The buffer is not laziness. Three constraints hold it up, and a replacement has to satisfy all three:

1. **The render loop must never block.** `drainStateSend` writes what the socket takes on `tick20ms`
   and leaves the rest, bounded by `drainChunkBytes()` (a fraction of the largest free block, floor
   2048, ceiling 65536).
2. **Clients drain at their own pace.** `stateSend_.sent[MAX_WS_CLIENTS]` is a per-client cursor over
   the same body; a slow client lags without holding the others.
3. **A message stays atomic per client.** While a state send is active, patch and WLED pushes to
   `/ws` are skipped so nothing interleaves inside one WebSocket message.

## Approach: fragment the message, keep one modest buffer

WebSocket messages may be split across frames: a first frame carrying the real opcode with FIN
clear, then continuation frames (opcode 0x0), the last with FIN set. The browser reassembles them
into one `message` event, so **the client needs no change at all**.

The state is then produced and sent in slices of a few KB. The peak allocation becomes one slice
rather than the whole document, which removes the contiguous-block requirement entirely.

The one hard question is that `buildStateJson` is a single forward walk of the module tree: it cannot
be resumed from an arbitrary byte offset. Three ways to reconcile that with per-client cursors, and
the plan takes the third:

- **A. Serialize per client, straight to the socket.** `JsonSink` already has socket mode. Simplest
  to write, but it re-walks the whole tree once per client and blocks on a slow socket, breaking
  constraint 1.
- **B. Resumable serialization.** Teach `buildStateJson` to stop at a slice boundary and resume from
  a saved position in the tree. No large buffer at all, but it means a serialization cursor
  (module index, control index, partial-value state) that has to stay correct while the tree can
  change underneath it. The most invasive option and the easiest to get subtly wrong.
- **C. Slice-at-a-time with a shared buffer (chosen).** Serialize into a fixed slice buffer, send
  that slice as one fragment to every client, and only then produce the next. Peak memory is one
  slice; the tree is walked once; no per-client re-serialization. The cost is that the slowest client
  paces the others, which is acceptable: the state frame is rare (connect and structural change) and
  a slow client already stalls behind its own cursor today.

## As built (2026-09-08)

The word "ring" in option C2 was misleading, and the design that landed is simpler. The constraint
was never TOTAL memory (the Dig-Octa has 62 KB free for a 44 KB document) but CONTIGUOUS memory (a
49 KB largest block against a doubling buffer that needs ~96 KB). So the document is serialized ONCE,
at push time, into a **chain of small slices**, each its own allocation of `drainChunkBytes()` bytes,
which any fragmented heap can serve. Nothing pauses the walk and nothing waits for a client.

- **`JsonSink` slice mode** (`JsonSink.h`): a caller-owned slice plus a `FlushFn`; a full slice is
  handed over and reused, `finish()` flushes the tail, a failed flush sets `overflowed()`.
- **`WsSliceChain`** (`WsSliceChain.h`, header-only like `JsonSink`): the flush target. Each slice
  carries `kHdrMax` bytes of frame-header room in front of its payload, so `finalize(opcode)` stamps
  the fragment headers in place (first: opcode with FIN clear; middle: 0x00; last: 0x80; a single
  slice: 0x81, byte-identical to the old framing) and every frame is one contiguous span. A per-client
  `Cursor` walks the chain; slices are shared, cursors are not. `writeWsFrameHeader` moved here.
- **`HttpServerModule`**: the push allocates one staging slice, serializes through it into
  `stateSend_.chain`, frees the staging slice, and arms `startStateChainSend()`. `drainStateSend`
  moves bytes from `remaining()` and `advance()`s; the chain is freed once every live client is done.
  A refused allocation mid-document clears the chain, warns, and clears `fullResyncPending_` so the
  value patches keep flowing (the early-return that froze the whole UI is the dead end above).

**Tests**: `unit_JsonSink_slices` (4 cases) and `unit_WsSliceChain` (6 cases: the exact frame
sequence, the single-slice framing unchanged, byte-for-byte reassembly at socket writes of 1, 7, 333
and 100000 bytes, eight clients at eight speeds on one chain, a refused allocation refusing to
finalize, clear-and-reuse). Both control-checked: sabotaging the tail flush or the last frame's FIN
fails exactly the test that pins it. 1,910 unit tests green.

**Bench** (Dig-Octa .181, WiFi, 65 KB free, 59 KB largest block, the board that lost its effect
cards): a raw WebSocket client received the state as **6 frames** (opcode 1 with FIN clear, four
continuations, FIN on the last: 5 x 7,935 + 4,864 bytes) and reassembled **44,539 bytes,
byte-identical to `/api/state`**, with both effects present. The first flash sent nothing: the
document holds formatted fragments over 256 bytes, and `appendf`'s long-fragment branch flagged
overflow in slice mode instead of flushing, so every chain was dropped. Fixed and pinned
(`unit_JsonSink_slices`, the long-fragment case).

## Steps

### 1. A slice-sized sink
`JsonSink` gains a mode that fills a caller-owned fixed buffer and, when full, hands it to a callback
before continuing. Fixed-buffer mode already exists and flags overflow instead of growing; this is
that path plus a flush hook, so no new buffer strategy is introduced. Slice size comes from
`drainChunkBytes()`, so a tight board takes small slices and a roomy one takes large.

### 2. Fragmented framing
`writeWsFrameHeader` already encodes any length; it needs the FIN bit and opcode as parameters
rather than always `0x81`. First slice: opcode 0x1, FIN clear. Middle: opcode 0x0, FIN clear. Last:
opcode 0x0, FIN set. A single-slice document keeps today's exact framing (0x81), so the common small
case is unchanged.

### 3. Slice-aware StateSend
`stateSend_` holds one slice plus its header, with the per-client cursor over THAT slice. When every
live client has drained it, the next slice is produced and the cursors reset. Add a
`slicesRemaining` / `lastSlice` flag so the drain knows when the message is complete and only then
clears `fullResyncPending_` and rebaselines the leaf hashes.

### 4. Failure paths
A client that dies mid-message is dropped as today. A client that connects DURING a send must not
receive a half message: it waits for the next full state (`fullResyncPending_` is set on connect
anyway). If a slice cannot be allocated at all, warn and keep the patch stream alive, which is the
lesson from the refusal that froze the UI.

## Tests

- `unit_JsonSink_slices`: a document larger than the slice buffer emits the expected sequence of
  slices, and reassembling them byte-for-byte equals the same document built in buffer mode.
- `unit_HttpServer_fragmented_state`: the frame sequence for a multi-slice document is
  `0x01 FIN=0`, `0x00 FIN=0`, ..., `0x00 FIN=1`, and a single-slice document is exactly `0x81 FIN=1`
  (today's framing, so small devices are untouched).
- A regression test for the original bug: a state document that exceeds any single allocation still
  arrives complete and parseable.
- Scenario: the existing WS state scenario re-run, with the observation block showing the tick cost
  did not regress.

## Verification

Desktop first. Then on the bench, both classic boards, since they bracket the problem:

1. **Dig-Octa (.181, WiFi, 57 KB max block, 44 KB state)**: the whole tree arrives, every effect card
   renders including the MoonLive one that vanished, and the header keeps updating.
2. **Olimex (.210, Ethernet, 102 KB max block)**: unchanged behavior, no regression on a board that
   was already comfortable.
3. A raw WebSocket client asserts the reassembled document is valid JSON and matches `/api/state`
   byte-for-byte.
4. Watch the tick cost on both: slicing runs on `tick20ms` and must not lengthen the render tick.

## Risks

- **A half-sent tree is worse than a truncated one.** If the last fragment is lost the browser holds
  an incomplete message forever. The FIN bookkeeping is the load-bearing part and the test above
  pins it directly.
- **The tree can change between slices.** A module added or removed mid-message would produce a
  document that is internally inconsistent. Mitigation: a structural change during a send sets
  `fullResyncPending_` again, so the next full state supersedes it, exactly as today.
- **Slower clients now pace each other.** Accepted, and reversible: per-client slice buffers would
  undo it at the cost of the memory this plan exists to save.

## Out of scope, worth noting

`LightPresets` alone is **13,086 bytes, 30% of the document**: one `list` control holding 13 built-in
presets that never change at runtime. Fetching it on demand rather than pushing it in every state
would cut the document by a third and help every board. Real, separate, and not a substitute for
streaming, since 31 KB still would not fit on a tight board.
