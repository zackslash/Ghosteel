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

Q_SIGNALS:
    void titleChanged(const QString &title);
    void bell();
    void desktopNotification(const QString &summary, const QString &body);

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
};

#endif // GHOSTTYVT_H
