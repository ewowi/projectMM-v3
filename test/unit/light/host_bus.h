#pragma once

// The host-bus contract, shared by every parallel peripheral's test file.
//
// `i80`, `MoonI80` and `Parlio` are three DMA peripherals for one job, and off-device that job is
// "hold a frame": the desktop platform backs all three with the same heap buffer
// (platform_desktop.cpp § Parallel-WS2812 buses). So the assertions are identical per peripheral,
// and writing them out per file produced two byte-identical ~50-line test cases differing only in
// a type name. Same shape as test/unit/core/conditional_controls.h — one behaviour, one home,
// called with the type under test.

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"

#include <cstdint>

namespace mm::test {

/// Pin the memory-backed bus contract for one peripheral type: allocation, that the encode path
/// can really write and read back, that an over-capacity transmit is refused rather than
/// truncated, and that busWait reports success.
template <typename Peripheral>
inline void checkHostBusAllocates() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);              // busInit reads the pin list through the owner
    drv.setPeripheralForTest(&peripheral);  // borrowed, not owned — no delete of a local

    REQUIRE(peripheral.busInit(768, /*wantSecondBuffer=*/false));
    CHECK(peripheral.busCapacity() == 768);

    uint8_t* buf = peripheral.busBuffer(0);
    REQUIRE(buf != nullptr);
    // Writable and reads back: the difference between a bus that carries the frame and one that
    // silently discards it.
    buf[0] = 0xA5;
    buf[767] = 0x5A;
    CHECK(buf[0] == 0xA5);
    CHECK(buf[767] == 0x5A);

    CHECK(peripheral.busTransmit(0, 768));
    // Truncating instead of refusing would look like a good frame while dropping every light past
    // the end of the buffer.
    CHECK_FALSE(peripheral.busTransmit(0, 4096));
    // Must be true: the driver reads a false as "the previous frame never completed" and holds the
    // next one back, stalling a bus that is never actually busy.
    CHECK(peripheral.busWait(0, 10));

    peripheral.busDeinit();
    CHECK(peripheral.busBuffer(0) == nullptr);
}

/// The DMA buffers must show up in the driver's memory readout.
///
/// They are allocated by the platform rather than by the driver, and were once left out of
/// driverHeapBytes() on that ownership argument. That made the card lie about the one thing a user
/// picks a driver on: the i80 frame is sized by the BUS WIDTH, not by the pins in use, so a
/// one-lane board pays the same fixed cost as an eight-lane one. On the bench (2026-09-08) a
/// one-lane board read 512 bytes for this driver against RmtLedDriver's 32 KB while actually
/// costing 49 KB more free heap, and nothing on screen said so.
template <typename Peripheral>
inline void checkHostBusCountedInHeapReadout() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);

    // Assert through dynamicBytes(), the PUBLIC readout the card renders: driverHeapBytes() is the
    // protected hook that feeds it, and pinning the visible number is what this test is for.
    drv.publishHeapBytesForTest();
    const size_t before = drv.dynamicBytes();

    REQUIRE(peripheral.busInit(4096, /*wantSecondBuffer=*/false));
    drv.publishHeapBytesForTest();
    const size_t single = drv.dynamicBytes();
    CHECK(single >= before + 4096);   // the frame is now visible, whoever allocated it

    // The second buffer is real memory too, and counting it is what tells a user that doubleBuffer
    // costs a whole extra frame. Measured against the SINGLE-buffered total rather than against
    // `single` plus a frame: `single` already carries the driver's other buffers, so the increment
    // to look for is one more frame on top of whatever the readout was.
    REQUIRE(peripheral.busInit(4096, /*wantSecondBuffer=*/true));
    drv.doubleBuffer = true;
    drv.publishHeapBytesForTest();
    const size_t doubled = drv.dynamicBytes();
    CHECK(doubled == single + 4096);   // exactly one extra frame, not a re-count of the whole driver

    peripheral.busDeinit();
}

/// Double-buffering lets the driver encode one frame while the other is in flight, so the two must
/// be distinct allocations — aliasing them would tear every frame.
template <typename Peripheral>
inline void checkHostBusDoubleBuffer() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);

    REQUIRE(peripheral.busInit(256, /*wantSecondBuffer=*/true));
    REQUIRE(peripheral.busBuffer(0) != nullptr);
    REQUIRE(peripheral.busBuffer(1) != nullptr);
    CHECK(peripheral.busBuffer(0) != peripheral.busBuffer(1));

    // Re-initialising single-buffered releases the second, rather than leaving a stale span a
    // later transmit could read from.
    REQUIRE(peripheral.busInit(256, /*wantSecondBuffer=*/false));
    CHECK(peripheral.busBuffer(1) == nullptr);
}

}  // namespace mm::test
