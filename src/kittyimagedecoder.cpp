#include "kittyimagedecoder.h"

#include <QImage>
#include <QDebug>

static const int KITTY_MAX_IMAGE_DIM = 4096;

static bool decodePngCallback(void* /*userdata*/,
                              const GhosttyAllocator* allocator,
                              const uint8_t* data, size_t data_len,
                              GhosttySysImage* out)
{
    // Load PNG from raw bytes
    QByteArray pngData(reinterpret_cast<const char*>(data), static_cast<int>(data_len));
    QImage img;
    if (!img.loadFromData(pngData, "PNG")) {
        qWarning() << "Kitty image decoder: failed to load PNG";
        return false;
    }

    // Cap dimensions to avoid GPU memory exhaustion
    if (img.width() > KITTY_MAX_IMAGE_DIM || img.height() > KITTY_MAX_IMAGE_DIM) {
        img = img.scaled(KITTY_MAX_IMAGE_DIM, KITTY_MAX_IMAGE_DIM,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Convert to RGBA8888
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
