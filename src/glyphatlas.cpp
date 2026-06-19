#include "glyphatlas.h"
#include <QDebug>
#include <QPainter>
#include <QFontMetrics>
#include <QOpenGLFunctions>
#include <QOpenGLContext>

GlyphAtlas::GlyphAtlas(int atlasWidth, int atlasHeight)
    : m_atlasWidth(atlasWidth)
    , m_atlasHeight(atlasHeight)
{
}

GlyphAtlas::~GlyphAtlas()
{
    if (QOpenGLContext::currentContext())
        destroy();
    else
        m_texture = 0; // Context gone — leak rather than crash
}

void GlyphAtlas::initialize()
{
    if (m_initialized)
        return;

    initializeOpenGLFunctions();

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_staging = QImage(m_atlasWidth, m_atlasHeight, QImage::Format_RGBA8888);
    m_staging.fill(Qt::transparent);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_atlasWidth, m_atlasHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, m_staging.constBits());

    m_initialized = true;

    // Reserve pixel (0,0) as transparent — empty cells will sample this
    m_packX = 1;
    m_packY = 0;
    m_rowHeight = 0;

    qDebug() << "GlyphAtlas: initialized" << m_atlasWidth << "x" << m_atlasHeight;
}

void GlyphAtlas::destroy()
{
    if (m_texture) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    m_cache.clear();
    m_initialized = false;
}

void GlyphAtlas::setFont(const QFont &font, int cellWidth, int cellHeight)
{
    m_font = font;
    m_fontBold = font;
    m_fontBold.setBold(true);
    m_fontItalic = font;
    m_fontItalic.setItalic(true);
    m_fontBoldItalic = font;
    m_fontBoldItalic.setBold(true);
    m_fontBoldItalic.setItalic(true);

    m_cellWidth = cellWidth;
    m_cellHeight = cellHeight;

    // Cache QFontMetrics for each variant to avoid per-glyph construction
    m_fm.reset(new QFontMetrics(m_font));
    m_fmBold.reset(new QFontMetrics(m_fontBold));
    m_fmItalic.reset(new QFontMetrics(m_fontItalic));
    m_fmBoldItalic.reset(new QFontMetrics(m_fontBoldItalic));
    m_ascent = m_fm->ascent();

    m_cache.clear();
    m_packX = 1;
    m_packY = 0;
    m_rowHeight = 0;

    if (m_initialized) {
        m_staging.fill(Qt::transparent);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_atlasWidth, m_atlasHeight,
                        GL_RGBA, GL_UNSIGNED_BYTE, m_staging.constBits());
    }
}

const GlyphInfo &GlyphAtlas::glyph(uint codepoint, bool bold, bool italic)
{
    GlyphKey key{codepoint, bold, italic};
    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return it.value();

    GlyphInfo info;
    rasterizeGlyph(codepoint, bold, italic, info);
    return m_cache.insert(key, info).value();
}

void GlyphAtlas::rasterizeGlyph(uint codepoint, bool bold, bool italic, GlyphInfo &info)
{
    const QFont &font = (bold && italic) ? m_fontBoldItalic
                      : bold ? m_fontBold
                      : italic ? m_fontItalic
                      : m_font;
    const QFontMetrics &fm = (bold && italic) ? *m_fmBoldItalic
                           : bold ? *m_fmBold
                           : italic ? *m_fmItalic
                           : *m_fm;

#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    int glyphWidth = fm.horizontalAdvance(QChar(codepoint));
#else
    int glyphWidth = fm.width(QChar(codepoint));
#endif
    int glyphHeight = fm.height();

    if (glyphWidth < 1) glyphWidth = 1;
    if (glyphHeight < 1) glyphHeight = 1;

    int x, y;
    if (!allocateSlot(glyphWidth, glyphHeight, x, y)) {
        // Atlas full — clear and re-rasterize lazily
        qWarning() << "GlyphAtlas: atlas full, clearing and reinitializing";
        clearAtlas();
        if (!allocateSlot(glyphWidth, glyphHeight, x, y)) {
            // Still can't fit — return placeholder
            info = {0, 0, 0, 0, m_cellWidth, m_ascent, 0, 0};
            return;
        }
    }

    QPainter painter(&m_staging);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.drawText(x, y + fm.ascent(), QChar(codepoint));
    painter.end();

    info.u0 = static_cast<float>(x) / m_atlasWidth;
    info.v0 = static_cast<float>(y) / m_atlasHeight;
    info.u1 = static_cast<float>(x + glyphWidth) / m_atlasWidth;
    info.v1 = static_cast<float>(y + glyphHeight) / m_atlasHeight;
    info.advance = glyphWidth;
    info.ascent = fm.ascent();
    info.width = glyphWidth;
    info.height = glyphHeight;

    // Upload the dirty region to GPU using reusable buffer (avoids QImage::copy allocation)
    Q_ASSERT(x + glyphWidth <= m_atlasWidth && y + glyphHeight <= m_atlasHeight);
    int bpp = 4; // RGBA8888
    int bytesPerLine = glyphWidth * bpp;
    m_uploadBuf.resize(bytesPerLine * glyphHeight);
    for (int row = 0; row < glyphHeight; row++) {
        const uchar *src = m_staging.constScanLine(y + row) + x * bpp;
        memcpy(m_uploadBuf.data() + row * bytesPerLine, src, bytesPerLine);
    }
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, glyphWidth, glyphHeight,
                    GL_RGBA, GL_UNSIGNED_BYTE, m_uploadBuf.constData());
}

bool GlyphAtlas::allocateSlot(int glyphWidth, int glyphHeight, int &x, int &y)
{
    // Simple shelf packing: advance cursor, wrap to next row when needed
    if (m_packX + glyphWidth > m_atlasWidth) {
        m_packX = 0;
        m_packY += m_rowHeight + 1; // 1px gap
        m_rowHeight = 0;
    }

    if (m_packY + glyphHeight > m_atlasHeight) {
        qWarning() << "GlyphAtlas: atlas full, cannot allocate" << glyphWidth << "x" << glyphHeight;
        return false;
    }

    x = m_packX;
    y = m_packY;
    m_packX += glyphWidth + 1; // 1px gap
    if (glyphHeight > m_rowHeight)
        m_rowHeight = glyphHeight;

    return true;
}

void GlyphAtlas::bind()
{
    glBindTexture(GL_TEXTURE_2D, m_texture);
}

void GlyphAtlas::clearAtlas()
{
    m_cache.clear();
    m_packX = 1;
    m_packY = 0;
    m_rowHeight = 0;
    m_staging.fill(Qt::transparent);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_atlasWidth, m_atlasHeight,
                    GL_RGBA, GL_UNSIGNED_BYTE, m_staging.constBits());
}
