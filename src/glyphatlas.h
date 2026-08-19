#ifndef GLYPHATLAS_H
#define GLYPHATLAS_H

#include <QOpenGLFunctions>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QHash>
#include <cstring>
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

// Cache key for multi-codepoint grapheme clusters (ZWJ families, flags,
// skin-tone sequences). Value semantics — no heap allocation for clusters
// up to INLINE_MAX codepoints. The longest common real-world cluster is
// an 8-codepoint skin-tone family. Pathological clusters (len > INLINE_MAX)
// bypass this cache and fall back to rendering buf[0] (current behavior).
struct ClusterKey {
    static constexpr int INLINE_MAX = 16;
    uint32_t codepoints[INLINE_MAX] = {};
    uint8_t  len  = 0;
    bool     bold = false;
    bool     italic = false;

    ClusterKey() = default;
    ClusterKey(const uint32_t *buf, uint32_t n, bool b, bool i)
        : bold(b), italic(i)
    {
        len = static_cast<uint8_t>((n <= INLINE_MAX) ? n : 0); // 0 = uncachable sentinel
        if (len > 0)
            memcpy(codepoints, buf, len * sizeof(uint32_t));
    }

    bool cacheable() const { return len >= 2; }

    bool operator==(const ClusterKey &o) const {
        if (len != o.len || bold != o.bold || italic != o.italic) return false;
        return memcmp(codepoints, o.codepoints, len * sizeof(uint32_t)) == 0;
    }
};

inline uint qHash(const ClusterKey &k, uint seed = 0) {
    uint h = seed ^ (uint(k.bold) << 8) ^ (uint(k.italic) << 9) ^ k.len;
    for (uint8_t i = 0; i < k.len; ++i)
        h ^= k.codepoints[i] + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

// Packed glyph info in atlas (UV rect only — the renderer consumes nothing else)
struct GlyphInfo {
    float u0, v0, u1, v1;
};

class GlyphAtlas : protected QOpenGLFunctions
{
public:
    // 2048² default: 1024² was too small for CJK working sets, causing thrashing
    GlyphAtlas(int atlasWidth = 2048, int atlasHeight = 2048);
    ~GlyphAtlas();

    void initialize();
    void destroy();

    void setFont(const QFont &font, int cellWidth, int cellHeight);

    // Get or rasterize a glyph. Returns texture coordinates.
    const GlyphInfo &glyph(uint codepoint, bool bold = false, bool italic = false);

    // Multi-codepoint grapheme cluster lookup. len must be >= 2; len == 1 must
    // call glyph() instead. Falls back to glyph(buf[0]) if len > INLINE_MAX.
    const GlyphInfo &glyphCluster(const uint32_t *buf, uint32_t len,
                                  bool bold = false, bool italic = false);

    void bind();

    // Incremented on every clearAtlas()/setFont(). Consumers can detect that
    // the atlas was wiped and re-packed mid-build (which invalidates UVs
    // computed before the wipe) by comparing epoch() before and after.
    int epoch() const { return m_epoch; }

private:
    void rasterizeGlyph(uint codepoint, bool bold, bool italic, GlyphInfo &info);
    void rasterizeCluster(const ClusterKey &key, GlyphInfo &info);
    // Shared rasterization core for single glyphs and grapheme clusters.
    GlyphInfo rasterizeText(const QString &text, bool bold, bool italic);
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

    // Packing state (simple shelf algorithm)
    int m_packX = 0;
    int m_packY = 0;
    int m_rowHeight = 0;

    // Bumped on every atlas wipe (clearAtlas/setFont) — see epoch().
    int m_epoch = 0;

    QHash<GlyphKey, GlyphInfo> m_cache;
    QHash<ClusterKey, GlyphInfo> m_clusterCache;

    // Small persistent scratch for glyphs and grapheme clusters; grown on demand, filled transparent per use.
    QImage m_glyphScratch;

    // Shared tightly-packed upload buffer; see rasterizeText() for the re-stride rationale.
    QByteArray m_uploadBuf;
};

#endif // GLYPHATLAS_H
