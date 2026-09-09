#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

// ColorLight 5A-75 wire format — the one place the layout lives, used by PanelCardDriver to build
// frames. Same shape as ArtNetPacket.h / DdpPacket.h, with one structural difference: this format
// has no receiver in the tree (the cards are the receiver), so there is no build→parse round-trip
// test. The unit tests pin the built bytes against the layout instead.
//
// **Raw Ethernet, not UDP.** These are complete L2 frames sent below IP: no address, no port, no
// DHCP lease. That is why the driver works on a link that never got one.
//
// **The EtherType field is overloaded, and it is the format's defining quirk.** A normal frame has
// a 2-byte EtherType at offset 12-13. Here byte 12 is the PACKET TYPE and byte 13 is already the
// first payload byte, so every packet's body starts at offset 13 rather than 14. A row packet
// therefore reads on the wire as EtherType 0x55<row-MSB> — not because the row is deliberately
// encoded into the EtherType, but because the row number is simply the first payload byte and lands
// there. Getting this wrong shifts every field by one and the cards silently discard the frame.
//
// **The MACs are fixed constants, not real addresses.** Destination 11:22:33:44:55:66, source
// 22:22:33:44:55:66 — the cards filter on that destination, so a frame sent to broadcast or from
// the device's own MAC is dropped without any visible error. The source is not this device's MAC.
//
// Frame layout, all packet types:
//   0-5    destination MAC — always kColorLightDestMac
//   6-11   source MAC — always kColorLightSrcMac
//   12     packet type (0x55 row data, 0x01 sync, 0x0A brightness, 0x07 discovery)
//   13+    payload, per type below
//
// Row data (type 0x55), payload at offset 13:
//   13-14  row number, big-endian
//   15-16  pixel offset within the row, big-endian
//   17-18  pixel count in this packet, big-endian
//   19     0x08 — constant; purpose undocumented
//   20     0x88 — constant; purpose undocumented
//   21+    pixel data, 3 bytes per pixel
//
// Sync (type 0x01), 112 bytes total, body zeroed except:
//   13     0x07 — "sent from a PC/netcard" (a hardware sender writes 0x00 here)
//   35     brightness          (payload byte 22)
//   36     0x05 — constant; purpose undocumented
//   38-40  brightness ×3       (payload bytes 25-27)
//
// Brightness (type 0x0A), 77 bytes total, body zeroed except:
//   13-15  brightness ×3
//   16     0xFF
//
// Per frame the order is: brightness, then every row ascending, then sync last — the sync is what
// latches the buffered rows onto the panels.
//
// No configuration packets are sent. The cards are configured by the vendor's own tool and keep
// that in flash; a sender only streams pixels.

// Fixed MACs. The cards filter on the destination, so these are not arbitrary.
constexpr uint8_t kColorLightDestMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
constexpr uint8_t kColorLightSrcMac[6]  = {0x22, 0x22, 0x33, 0x44, 0x55, 0x66};

// Byte 12 is the type; byte 13 begins the payload (see the EtherType note above).
constexpr size_t COLORLIGHT_TYPE_OFFSET = 12;
constexpr size_t COLORLIGHT_DATA_OFFSET = 13;

constexpr uint8_t COLORLIGHT_TYPE_ROW        = 0x55;
constexpr uint8_t COLORLIGHT_TYPE_SYNC       = 0x01;
constexpr uint8_t COLORLIGHT_TYPE_BRIGHTNESS = 0x0A;

// Row header: row, offset, count, two constants — 8 bytes, so pixels start at 13 + 8 = 21.
constexpr size_t COLORLIGHT_ROW_HEADER = 8;
constexpr size_t COLORLIGHT_ROW_PREFIX = COLORLIGHT_DATA_OFFSET + COLORLIGHT_ROW_HEADER;  // 21

constexpr uint16_t COLORLIGHT_MAX_PIXELS_PER_PACKET = 497;
constexpr uint8_t COLORLIGHT_BYTES_PER_PIXEL = 3;

// Largest frame built: 21 + 497x3 = 1512 bytes. Above the 1500-byte MTU on purpose: these are raw
// L2 frames on a dedicated link to dumb receivers, never IP packets a router would fragment.
//
// FPP packs rows the same way. Harald Kubota's write-up sends 128 pixels per packet instead (391
// bytes), which stays inside the MTU: both work against the cards, and the larger packet is fewer
// frames for the same wall. Worth knowing if a wall ever goes dark through a SWITCH that will not
// pass a 1512-byte frame, since nothing in the path reports that: the card simply never sees a row
// and its activity LED stays still, which looks exactly like a transmit path that is not running.
constexpr size_t COLORLIGHT_MAX_FRAME =
    COLORLIGHT_ROW_PREFIX + COLORLIGHT_MAX_PIXELS_PER_PACKET * COLORLIGHT_BYTES_PER_PIXEL;  // 1512

constexpr size_t COLORLIGHT_SYNC_FRAME = 112;
constexpr size_t COLORLIGHT_BRIGHTNESS_FRAME = 77;

// Constants the cards require whose purpose is not documented anywhere verifiable. Reproduced from
// the observed layout and named to say exactly that.
constexpr uint8_t kUnknownRowConst0 = 0x08;   // row payload byte 6
constexpr uint8_t kUnknownRowConst1 = 0x88;   // row payload byte 7
constexpr uint8_t kSyncFromNetcard  = 0x07;   // sync payload byte 0: sender is a PC/netcard
constexpr uint8_t kUnknownSyncConst = 0x05;   // sync payload byte 23

// Write the 12-byte MAC pair plus the packet-type byte. Everything after byte 12 is payload.
inline void buildColorLightHeader(uint8_t* out, uint8_t packetType) {
    std::memcpy(out, kColorLightDestMac, 6);
    std::memcpy(out + 6, kColorLightSrcMac, 6);
    out[COLORLIGHT_TYPE_OFFSET] = packetType;
}

// Build one row-data frame: up to COLORLIGHT_MAX_PIXELS_PER_PACKET pixels of one row, starting at
// `pixelOffset` within that row. Returns the total frame length.
//
// `out` must hold COLORLIGHT_ROW_PREFIX + pixelCount × 3 bytes; COLORLIGHT_MAX_FRAME covers the
// largest. Pixel bytes are copied verbatim — channel order is the caller's business (the driver's
// Correction has already applied it), so this never reorders them.
inline size_t buildColorLightRowPacket(uint8_t* out, uint16_t row,
                                       uint16_t pixelOffset, uint16_t pixelCount,
                                       const uint8_t* pixels) {
    buildColorLightHeader(out, COLORLIGHT_TYPE_ROW);
    uint8_t* data = out + COLORLIGHT_DATA_OFFSET;
    data[0] = static_cast<uint8_t>(row >> 8);
    data[1] = static_cast<uint8_t>(row & 0xFF);
    data[2] = static_cast<uint8_t>(pixelOffset >> 8);
    data[3] = static_cast<uint8_t>(pixelOffset & 0xFF);
    data[4] = static_cast<uint8_t>(pixelCount >> 8);
    data[5] = static_cast<uint8_t>(pixelCount & 0xFF);
    data[6] = kUnknownRowConst0;
    data[7] = kUnknownRowConst1;
    const size_t dataBytes = static_cast<size_t>(pixelCount) * COLORLIGHT_BYTES_PER_PIXEL;
    if (pixels && dataBytes) std::memcpy(out + COLORLIGHT_ROW_PREFIX, pixels, dataBytes);
    return COLORLIGHT_ROW_PREFIX + dataBytes;
}

// Build the sync frame that latches every row sent since the last one. Fixed 112 bytes, zeroed
// except for the netcard marker, one undocumented constant, and brightness in four places.
// Returns the frame length; `out` must hold COLORLIGHT_SYNC_FRAME bytes.
inline size_t buildColorLightSyncPacket(uint8_t* out, uint8_t brightness) {
    std::memset(out, 0, COLORLIGHT_SYNC_FRAME);
    buildColorLightHeader(out, COLORLIGHT_TYPE_SYNC);
    uint8_t* data = out + COLORLIGHT_DATA_OFFSET;
    data[0]  = kSyncFromNetcard;
    data[22] = brightness;
    data[23] = kUnknownSyncConst;
    data[25] = brightness;
    data[26] = brightness;
    data[27] = brightness;
    return COLORLIGHT_SYNC_FRAME;
}

// Build the brightness frame: one level on each of the three channels. Sent first in a frame, ahead
// of the rows. Reported not to work on older card firmware, so the driver treats it as advisory and
// never depends on it having landed. Returns the frame length; `out` must hold
// COLORLIGHT_BRIGHTNESS_FRAME bytes.
inline size_t buildColorLightBrightnessPacket(uint8_t* out, uint8_t brightness) {
    std::memset(out, 0, COLORLIGHT_BRIGHTNESS_FRAME);
    buildColorLightHeader(out, COLORLIGHT_TYPE_BRIGHTNESS);
    uint8_t* data = out + COLORLIGHT_DATA_OFFSET;
    data[0] = brightness;
    data[1] = brightness;
    data[2] = brightness;
    data[3] = 0xFF;
    return COLORLIGHT_BRIGHTNESS_FRAME;
}

}  // namespace mm
