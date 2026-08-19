#include "kittyimagedecoder.h"
#include "kittyimageguard.h"

#include <QImage>
#include <QBuffer>
#include <QImageReader>
#include <QDebug>
#include <algorithm>
#include <limits>

// Images up to this many pixels on each side decode at native resolution and
// fit within typical Sailfish GLES GL_MAX_TEXTURE_SIZE (2048-4096).
static const int KITTY_MAX_IMAGE_DIM = 4096;

// Qt's PNG streaming downscaler and libpng allocate row buffers proportional to
// the native WIDTH; cap it so a pathological wide image can't OOM those buffers
// before the downscale runs. Only the width matters (height just lengthens the
// row loop and adds no per-row allocation).
static const uint32_t KITTY_MAX_NATIVE_WIDTH = 16384;

static bool decodePngCallback(void* /*userdata*/,
                              const GhosttyAllocator* allocator,
                              const uint8_t* data, size_t data_len,
                              GhosttySysImage* out)
{
    // Guard against data_len exceeding QByteArray's int-based size
    if (data_len > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    const KittyPngHeader hdr = kittyPngSniffHeader(data, data_len);
    const bool oversized =
        hdr.valid && (hdr.width > uint32_t(KITTY_MAX_IMAGE_DIM)
                      || hdr.height > uint32_t(KITTY_MAX_IMAGE_DIM));

    QImage img;
    if (oversized) {
        // Qt 5.6 can stream-downscale RGB/RGBA non-interlaced PNGs via
        // QImageReader::setScaledSize: read_image_scaled() decodes row by row
        // into width-proportional buffers, never allocating the full W*H*4.
        // Other PNG kinds (grayscale, palette, interlaced) don't take that
        // streaming path in 5.6 and would decode at full size, OOMing on a bomb,
        // so those are rejected.
        const bool streamable = (hdr.colorType == 2 || hdr.colorType == 6)
                                && hdr.interlaceMethod == 0
                                && hdr.width > 0 && hdr.height > 0
                                && hdr.width <= KITTY_MAX_NATIVE_WIDTH;
        if (!streamable) {
            qWarning("Kitty image decoder: PNG %ux%u (color_type=%u interlace=%u)"
                     " exceeds %dpx and can't be safely downscaled; rejecting",
                     hdr.width, hdr.height, hdr.colorType, hdr.interlaceMethod,
                     KITTY_MAX_IMAGE_DIM);
            return false;
        }
        // Fit within KITTY_MAX_IMAGE_DIM on each side, preserving aspect ratio.
        // The output is then <= 4096*4096*4 = 64 MiB and within GL_MAX_TEXTURE_SIZE.
        const double scale = std::min(double(KITTY_MAX_IMAGE_DIM) / double(hdr.width),
                                      double(KITTY_MAX_IMAGE_DIM) / double(hdr.height));
        const int sw = std::max<int>(1, int(hdr.width * scale));
        const int sh = std::max<int>(1, int(hdr.height * scale));

        // Caveat: a kitty placement carrying an explicit source rectangle in
        // original pixel coords will be slightly off against this smaller image.
        // Real clients (kitten icat, viu) place whole images, so this is rare.

        QBuffer pngBuf;
        pngBuf.setData(reinterpret_cast<const char*>(data), static_cast<int>(data_len));
        pngBuf.open(QIODevice::ReadOnly);
        QImageReader reader(&pngBuf, "PNG");
        reader.setScaledSize(QSize(sw, sh));
        if (!reader.read(&img)) {
            qWarning("Kitty image decoder: scaled decode %ux%u -> %dx%d failed: %s",
                     hdr.width, hdr.height, sw, sh,
                     qPrintable(reader.errorString()));
            return false;
        }
    } else {
        // Within the dim cap, or non-PNG / un-sniffable input: original path.
        QByteArray pngData(reinterpret_cast<const char*>(data), static_cast<int>(data_len));
        if (!img.loadFromData(pngData, "PNG")) {
            qWarning() << "Kitty image decoder: failed to load PNG";
            return false;
        }
    }

    img = img.convertToFormat(QImage::Format_RGBA8888);

    const int w = img.width();
    const int h = img.height();
    const size_t pixelBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;

    // Allocate via Ghostty's allocator
    uint8_t* buf = ghostty_alloc(allocator, pixelBytes);
    if (!buf) {
        qWarning() << "Kitty image decoder: allocation failed";
        return false;
    }

    // QImage may have bytesPerLine > width*4 (stride padding).
    // Copy row by row if needed.
    const int srcStride = img.bytesPerLine();
    const int dstStride = w * 4;
    if (srcStride == dstStride) {
        memcpy(buf, img.constBits(), pixelBytes);
    } else {
        for (int y = 0; y < h; ++y) {
            memcpy(buf + y * dstStride,
                   img.constBits() + y * srcStride,
                   dstStride);
        }
    }

    out->width = static_cast<uint32_t>(w);
    out->height = static_cast<uint32_t>(h);
    out->data = buf;
    out->data_len = pixelBytes;
    return true;
}

void kittyImageDecoderRegister()
{
    ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,
                    reinterpret_cast<const void*>(decodePngCallback));
}
