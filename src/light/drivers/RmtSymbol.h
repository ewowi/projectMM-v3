#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// The ESP32 RMT bit-shape word: domain logic, no ESP header, so it is host-testable without an
// ESP32 (the platform owns only the peripheral that expands wire bytes with these shapes; see
// platform.h rmtWs2812SetBitTiming).
//
// RMT symbol layout (matches ESP-IDF's rmt_symbol_word_t, documented here so no
// driver/rmt_*.h leaks into src/light/): one 32-bit word is two 16-bit halves,
// each a (duration:15, level:1) pair with the level in bit 15:
//
//   bits  0..14 : duration0 (RMT ticks)      bit 15 : level0 (0 or 1)
//   bits 16..30 : duration1 (RMT ticks)      bit 31 : level1 (0 or 1)
//
// One WS2812 data bit = one symbol: HIGH for t?hTicks, then LOW for the rest of
// the cell. So half0 = (t?hTicks, level 1), half1 = (period - t?h, level 0).
constexpr uint32_t makeRmtSymbol(uint16_t dur0, uint8_t lvl0,
                                 uint16_t dur1, uint8_t lvl1) {
    return (static_cast<uint32_t>(dur0 & 0x7FFF))
         | (static_cast<uint32_t>(lvl0 & 1) << 15)
         | (static_cast<uint32_t>(dur1 & 0x7FFF) << 16)
         | (static_cast<uint32_t>(lvl1 & 1) << 31);
}

} // namespace mm
