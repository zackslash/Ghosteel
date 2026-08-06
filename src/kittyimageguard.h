#ifndef KITTYIMAGEGUARD_H
#define KITTYIMAGEGUARD_H

#include <cstddef>
#include <cstdint>

// PNG IHDR sniffer: guards the kitty image decoder against decompression bombs
// and lets oversized-but-legitimate images be downscaled instead of rejected.
//
// A PNG declares its pixel dimensions (plus color type and interlace method) in
// the 13-byte IHDR chunk data, and a decoder allocates the destination pixel
// buffer from the declared dimensions before inflating the compressed stream.
// A malicious PNG can therefore declare huge dimensions with a tiny payload and
// force a multi-GB allocation. Sailfish Qt 5.6 has no
// QImageReader::setAllocationLimit (added in Qt 5.13), so this header sniff is
// the only reliable pre-decode guard.
//
// PNG layout read here:
//   bytes  0..7   signature (89 50 4E 47 0D 0A 1A 0A)
//   bytes  8..11  IHDR chunk length (always 13, not validated)
//   bytes 12..15  chunk type "IHDR"
//   bytes 16..19  width  (big-endian uint32)
//   bytes 20..23  height (big-endian uint32)
//   byte  24      bit depth
//   byte  25      color type (0=gray, 2=RGB, 3=palette, 4=gray+alpha, 6=RGBA)
//   byte  26      compression method
//   byte  27      filter method
//   byte  28      interlace method (0=none, 1=Adam7)
//
// Returns a header with valid=true only when `data` is a recognisable PNG with
// a complete IHDR (>= 29 bytes). Callers use width/height to decide whether the
// image fits a decode budget, and colorType/interlaceMethod to decide whether
// Qt can stream-downscale it (RGB/RGBA, non-interlaced) instead of allocating
// the full W*H*4 buffer up front.
struct KittyPngHeader {
    bool     valid           = false;
    uint32_t width           = 0;
    uint32_t height          = 0;
    uint8_t  colorType       = 0;
    uint8_t  interlaceMethod = 0;
};

inline KittyPngHeader kittyPngSniffHeader(const uint8_t *data, size_t len)
{
    KittyPngHeader h;
    static const uint8_t kPngSig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    if (!data || len < 29)
        return h;

    for (int i = 0; i < 8; ++i)
        if (data[i] != kPngSig[i])
            return h;

    if (data[12] != 0x49 || data[13] != 0x48
        || data[14] != 0x44 || data[15] != 0x52)
        return h;

    h.width           = (static_cast<uint32_t>(data[16]) << 24)
                      | (static_cast<uint32_t>(data[17]) << 16)
                      | (static_cast<uint32_t>(data[18]) << 8)
                      |  static_cast<uint32_t>(data[19]);
    h.height          = (static_cast<uint32_t>(data[20]) << 24)
                      | (static_cast<uint32_t>(data[21]) << 16)
                      | (static_cast<uint32_t>(data[22]) << 8)
                      |  static_cast<uint32_t>(data[23]);
    h.colorType       = data[25];
    h.interlaceMethod = data[28];
    h.valid           = true;
    return h;
}

#endif // KITTYIMAGEGUARD_H
