#pragma once

// Reading an ESP32 firmware image's header, without ESP-IDF.
//
// The layout is a fixed on-disk format the bootloader parses, so it is the same bytes whether
// they arrive on a device or in a test: magic, chip id, then the app descriptor carrying the
// project name and version. Defining it here rather than including esp_app_format.h is what
// lets the vetting run in a host test, which matters because the code it guards ERASES a
// device's only recovery image. The layout is pinned by unit_FirmwareImage against real
// binaries built by the ESP32 toolchain.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::firmware {

/// First byte of every ESP32 image.
constexpr uint8_t kImageMagic = 0xE9;
/// Marks the app descriptor that follows the image and first segment headers.
constexpr uint32_t kDescMagic = 0xABCD5432;
/// Offset of the app descriptor: a 24-byte image header plus an 8-byte segment header.
constexpr size_t kDescOffset = 32;
/// Offset of the chip id within the image header.
constexpr size_t kChipIdOffset = 12;
/// Enough bytes to identify an image: through the descriptor's version and project name.
constexpr size_t kIdentifyBytes = kDescOffset + 96;

/// Chip ids, as the image header encodes them (esp_chip_id_t).
/// Every id esp_chip_id_t defines, not only the ones that ship MoonBase today: a missing entry
/// makes a valid image land on Invalid, so it is still refused but described as "for another
/// chip" when the truth is "for a chip this table does not name". S31 is already built here.
enum class ChipId : uint16_t {
    Esp32    = 0x0000,
    Esp32S2  = 0x0002,
    Esp32C3  = 0x0005,
    Esp32S3  = 0x0009,
    Esp32C2  = 0x000C,
    Esp32C6  = 0x000D,
    Esp32H2  = 0x0010,
    Esp32P4  = 0x0012,
    Esp32C5  = 0x0017,
    Esp32H4  = 0x001C,
    Esp32S31 = 0x0020,
    Invalid  = 0xFFFF,
};

/// What an image says about itself. Empty strings when the descriptor is absent or unreadable.
struct ImageInfo {
    bool   valid       = false;   ///< begins with the image magic
    bool   described   = false;   ///< carries a readable app descriptor
    ChipId chip        = ChipId::Invalid;
    char   project[32] = {};      ///< "projectMM" or "projectMM-moonbase"
    char   version[32] = {};
};

/// Read what `buf` claims to be. Reads only the first kIdentifyBytes and never past `len`, so a
/// truncated or hostile stream reports what it can rather than reading past its own buffer.
inline ImageInfo identify(const uint8_t* buf, size_t len) {
    ImageInfo out;
    if (!buf || len == 0) return out;
    if (buf[0] != kImageMagic) return out;
    out.valid = true;
    if (len >= kChipIdOffset + 2) {
        uint16_t id = 0;
        std::memcpy(&id, buf + kChipIdOffset, 2);
        out.chip = static_cast<ChipId>(id);
    }
    if (len < kDescOffset + 96) return out;
    uint32_t magic = 0;
    std::memcpy(&magic, buf + kDescOffset, 4);
    if (magic != kDescMagic) return out;
    out.described = true;
    // The descriptor's version is at +16 and its project name at +48, each a fixed 32-byte field
    // that IDF may leave unterminated when the string exactly fills it.
    std::memcpy(out.version, buf + kDescOffset + 16, 31);
    std::memcpy(out.project, buf + kDescOffset + 48, 31);
    return out;
}

/// Is this a MoonBase image for `chip`? Returns the reason it is not, for a caller that has to
/// tell a person which file they should have picked.
///
/// The three ways to be wrong, in the order they are cheapest to detect:
///   not an image      a 404 body, an HTML error page, a .zip
///   the wrong chip    one MoonBase per chip, one paste apart, and a checksum will NOT catch it
///   not MoonBase      an app image, which sits beside it on the releases page
inline const char* moonBaseRejection(const ImageInfo& info, ChipId chip) {
    if (!info.valid)     return "not a firmware image";
    if (info.chip != chip) return "image is for another chip";
    if (!info.described) return "image carries no description";
    if (std::strcmp(info.project, "projectMM-moonbase") != 0) return "not a MoonBase image";
    return nullptr;
}

} // namespace mm::firmware
