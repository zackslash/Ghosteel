#ifndef GLYPHATLAS_H
#define GLYPHATLAS_H

#include <QOpenGLFunctions>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QHash>
#include <memory>

// Glyph key: codepoint + font variant
struct GlyphKey {
    uint codepoint;
    bool bold;
    bool italic;
    bool operator==(const GlyphKey &o) const {
        return codepoint == o.codepoint && bold == o.bold && italic == o.italic;
    }
};
inline uint qHash(const GlyphKey &key, uint seed = 0) {
    return qHash(uint(key.codepoint) | (uint(key.bold) << 30) | (uint(key.italic) << 31), seed);
}

// Packed glyph info in atlas
struct GlyphInfo {
    float u0, v0, u1, v1;
    int advance;
    int ascent;
    int width, height;
};

class GlyphAtlas : protected QOpenGLFunctions
{
public:
    // 2048² default: 1024² was too small for CJK working sets, causing thrashing
    GlyphAtlas(int atlasWidth = 2048, int atlasHeight = 2048);
    ~GlyphAtlas();

    void initialize();
    void destroy();

    // Set the font for rasterization
    void setFont(const QFont &font, int cellWidth, int cellHeight);

    // Get or rasterize a glyph. Returns texture coordinates.
    const GlyphInfo &glyph(uint codepoint, bool bold = false, bool italic = false);

    void bind();

    int width() const { return m_atlasWidth; }
    int height() const { return m_atlasHeight; }

    // Cell dimensions (from font metrics)
    int cellWidth() const { return m_cellWidth; }
    int cellHeight() const { return m_cellHeight; }
    int ascent() const { return m_ascent; }

private:
    void rasterizeGlyph(uint codepoint, bool bold, bool italic, GlyphInfo &info);
    bool allocateSlot(int glyphWidth, int glyphHeight, int &x, int &y);
    void clearAtlas();

    int m_atlasWidth;
    int m_atlasHeight;
    GLuint m_texture = 0;
    bool m_initialized = false;

    QFont m_font;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;
    std::unique_ptr<QFontMetrics> m_fm;
    std::unique_ptr<QFontMetrics> m_fmBold;
    std::unique_ptr<QFontMetrics> m_fmItalic;
    std::unique_ptr<QFontMetrics> m_fmBoldItalic;
    int m_cellWidth = 0;
    int m_cellHeight = 0;
    int m_ascent = 0;

    // Packing state (simple shelf algorithm)
    int m_packX = 0;
    int m_packY = 0;
    int m_rowHeight = 0;

    QHash<GlyphKey, GlyphInfo> m_cache;

    // Small persistent scratch for single-glyph rasterization; grown on demand, filled transparent per use.
    QImage m_glyphScratch;

    // Tightly-packed per-glyph upload buffer. GL reads glyphWidth*4 bytes per row;
    // m_glyphScratch is reused wider than many glyphs, so we must re-stride here.
    QByteArray m_uploadBuf;
};

#endif // GLYPHATLAS_H
