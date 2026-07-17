#ifndef KITTYIMAGEGUARD_H
#define KITTYIMAGEGUARD_H

#include <cstddef>
#include <cstdint>

// PNG IHDR dimension cap — guards the kitty image decoder against decompression bombs.
//
// A PNG declares its pixel dimensions in the IHDR chunk as two big-endian uint32
// values, and a decoder allocates the destination pixel buffer from those
// dimensions before inflating the compressed stream. A malicious PNG can therefore
// declare huge dimensions while shipping a tiny compressed payload, forcing Qt's
// QImage::loadFromData to allocate gigabytes before the 4096 px cap in the kitty
// image decoder ever runs. Sailfish Qt 5.6 has no QImageReader::setAllocationLimit
// (added in Qt 5.13), so this header sniff is the only reliable pre-decode guard.
//
// PNG layout read here:
//   bytes  0..7   signature (89 50 4E 47 0D 0A 1A 0A)
//   bytes  8..11  IHDR chunk length (always 13, not validated here)
//   bytes 12..15  chunk type "IHDR"
//   bytes 16..19  width  (big-endian uint32)
//   bytes 20..23  height (big-endian uint32)
//
// Returns true only when `data` is recognisably a PNG whose declared width OR
// height exceeds `maxDim` — i.e. the caller should reject it before decode.
// Returns false for non-PNG data, truncated headers, or PNGs within the cap, so
// the caller can let its normal decode path handle those (preserving existing
// failure diagnostics for non-PNG / corrupt input).
inline bool kittyPngExceedsDimCap(const uint8_t *data, size_t len, int maxDim)
{
    static const uint8_t kPngSig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    if (!data || len < 24)
        return false;

    for (int i = 0; i < 8; ++i)
        if (data[i] != kPngSig[i])
            return false;

    if (data[12] != 0x49 || data[13] != 0x48
        || data[14] != 0x44 || data[15] != 0x52)
        return false;

    const uint32_t width = (static_cast<uint32_t>(data[16]) << 24)
                         | (static_cast<uint32_t>(data[17]) << 16)
                         | (static_cast<uint32_t>(data[18]) << 8)
                         |  static_cast<uint32_t>(data[19]);
    const uint32_t height = (static_cast<uint32_t>(data[20]) << 24)
                          | (static_cast<uint32_t>(data[21]) << 16)
                          | (static_cast<uint32_t>(data[22]) << 8)
                          |  static_cast<uint32_t>(data[23]);

    const uint32_t limit = static_cast<uint32_t>(maxDim);
    return width > limit || height > limit;
}

#endif // KITTYIMAGEGUARD_H
