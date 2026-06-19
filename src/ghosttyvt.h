#ifndef GHOSTTYVT_H
#define GHOSTTYVT_H

// Qt defines `emit` as an empty macro, which conflicts with the
// Ghostty C API using 'emit' as a struct field name.
// Undefine BEFORE including Ghostty headers to handle both:
//   1. Normal compilation (we include Ghostty before Qt — no conflict)
//   2. moc-generated code (moc includes Qt headers BEFORE this file)
#ifdef emit
#undef emit
#endif

#include <ghostty/vt.h>

#include <QObject>
#include <functional>
#include <QStringList>

// Thread safety: This class is NOT thread-safe. All methods and callbacks
// (including writePtyCallback) run on the main GUI thread. The PtyReaderThread
// only does blocking read() and delivers data to the main thread via
// dataReady signal with an explicit Qt::QueuedConnection.

class GhosttyVt : public QObject
{
    Q_OBJECT
public:
    // OSC 777 desktop notification scanner states
    enum Osc777State {
        OSC777_IDLE,       // Waiting for ESC
        OSC777_ESC,        // Found ESC, expecting ']'
        OSC777_BRACKET,    // Found ']', expecting '7'
        OSC777_7A,         // Found first '7', expecting second '7'
        OSC777_7B,         // Found second '7', expecting third '7'
        OSC777_SEMI1,      // Found "777", expecting ';'
        OSC777_NOTIFY,     // Matching "notify;"
        OSC777_TITLE,      // Reading title until ';'
        OSC777_BODY,       // Reading body until BEL (0x07)
    };

    // OSC 52 clipboard scanner states
    enum Osc52State {
        OSC52_IDLE,        // Waiting for ESC
        OSC52_ESC,         // Found ESC, expecting ']'
        OSC52_BRACKET,     // Found ']', expecting '5'
        OSC52_FIVE,        // Found '5', expecting '2'
        OSC52_TWO,         // Found '2', expecting ';'
        OSC52_SEMI,        // Found "52", expecting ';'
        OSC52_KIND,        // Reading selection target (c/s/p) until ';'
        OSC52_DATA,        // Reading base64 payload until BEL or ST
        OSC52_ST_ESC,      // Found ESC in DATA — expecting '\' for ST terminator
    };

    using PtyWriteFn = std::function<void(const char *, size_t)>;

    explicit GhosttyVt(QObject *parent = nullptr);
    ~GhosttyVt();

    bool create(uint16_t cols, uint16_t rows, PtyWriteFn writeFn);
    void destroy();

    // Must be called from main thread only.
    void vtWrite(const uint8_t *data, size_t len);
    void updateRenderState();

    GhosttyRenderState renderState() const { return m_renderState; }
    GhosttyTerminal terminal() const { return m_terminal; }

    QByteArray encodeKeyEvent(GhosttyKey key, GhosttyKeyAction action,
                              GhosttyMods mods, const char *utf8, size_t utf8Len);

    bool isMouseTracking() const;
    QByteArray encodeMouseEvent(GhosttyMouseAction action,
                                GhosttyMouseButton button,
                                float x, float y, GhosttyMods mods);
    void updateMouseEncoderSize(uint32_t screenW, uint32_t screenH,
                                uint32_t cellW, uint32_t cellH,
                                uint32_t paddingTop);
    void setMouseButtonPressed(bool pressed);
    QStringList extractSearchText();
    bool isSearchTextDirty() const { return m_searchTextDirty; }

    // Scrollback persistence — export terminal content (scrollback + active) as
    // VT sequences that can be replayed to restore the terminal state.
    // Returns empty if on alternate screen (TUI apps) or terminal is null.
    QByteArray exportScrollback(uint16_t &outCols, uint16_t &outRows) const;

    // Restore scrollback from a previously exported byte array.
    // The data must be in the format produced by exportScrollback() (header + VT).
    // Replay saved VT data into the terminal. Safe to call after create()
    // and same-dimension resize (setupTerminal does this correctly).
    void restoreScrollback(const QByteArray &data, uint16_t actualCols);

    // Returns the hyperlink URI for the cell at viewport (col, row).
    // Returns empty string if no hyperlink. Uses GHOSTTY_POINT_TAG_VIEWPORT.
    QString getHyperlinkAt(uint16_t col, uint32_t row) const;

    // Returns true if the cell at (col, row) is a wide-character spacer
    // (the invisible second half of a CJK/emoji character).
    static bool isWideCharSpacer(GhosttyTerminal terminal, uint16_t col, uint32_t row);

Q_SIGNALS:
    void titleChanged(const QString &title);
    void bell();
    void desktopNotification(const QString &summary, const QString &body);
    void clipboardWriteRequest(const QByteArray &base64Data, const QString &kind);
    void clipboardReadRequest(const QString &kind);

private:
    static void writePtyCallback(GhosttyTerminal t, void *ud,
                                 const uint8_t *data, size_t len);
    static void titleChangedCallback(GhosttyTerminal t, void *ud);
    static void bellCallback(GhosttyTerminal t, void *ud);

    GhosttyTerminal m_terminal = nullptr;
    GhosttyRenderState m_renderState = nullptr;
    GhosttyKeyEncoder m_keyEncoder = nullptr;
    GhosttyMouseEncoder m_mouseEncoder = nullptr;
    PtyWriteFn m_ptyWriteFn;
    bool m_needsEncoderSync = true; // Only sync encoders when terminal modes change
    bool m_searchTextDirty = true; // Set in vtWrite(), cleared by extractSearchText()

    // OSC 777 desktop notification scanner
    Osc777State m_osc777State = OSC777_IDLE;
    int m_osc777NotifyIdx = 0;
    QByteArray m_osc777Title;
    QByteArray m_osc777Body;

    // OSC 52 clipboard scanner
    Osc52State m_osc52State = OSC52_IDLE;
    QByteArray m_osc52Kind;     // Selection target: "c", "s", "p", etc.
    QByteArray m_osc52Data;     // Base64 payload accumulator
    static const int MaxOsc52KindLen = 16;
    static const int MaxOsc52DataLen = 1024 * 1024; // 1MB base64 cap
};

#endif // GHOSTTYVT_H
