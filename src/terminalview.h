#ifndef TERMINALVIEW_H
#define TERMINALVIEW_H

#include <QQuickPaintedItem>
#include <QImage>
#include <QFont>
#include <QElapsedTimer>

#include "ghosttyvt.h"

class PtyManager;

class TerminalView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int stickyModifiers READ stickyModifiers WRITE setStickyModifiers NOTIFY stickyModifiersChanged)
public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView();

    int fontSize() const { return m_fontSize; }
    void setFontSize(int size);
    QString title() const { return m_title; }
    int stickyModifiers() const { return m_stickyModifiers; }
    void setStickyModifiers(int mods);

    Q_INVOKABLE void paste();         // Paste from system clipboard
    Q_INVOKABLE void copySelection(); // Copy terminal selection to clipboard
    Q_INVOKABLE void sendKey(int qtKey, int modifiers = 0);
    Q_INVOKABLE void restartShell();  // Restart shell after exit
    Q_INVOKABLE void setActive(bool active); // Start/stop blink timer
    void cleanup();                   // Stop PTY/threads before destruction

Q_SIGNALS:
    void fontSizeChanged();
    void titleChanged();
    void stickyModifiersChanged();
    void terminalBell();
    void desktopNotification(const QString &summary, const QString &body);

protected:
    void paint(QPainter *painter) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void touchEvent(QTouchEvent *event) override;

private slots:
    void onPtyData(const QByteArray &data);
    void onShellExited(int exitCode);

private:
    void setupTerminal();
    void applyColorScheme();
    void recalculateDimensions();
    void renderCells(QPainter *painter);
    void sendKeyEvent(GhosttyKey key, GhosttyKeyAction action,
                      GhosttyMods mods, const QString &text);
    GhosttyKey mapQtKey(int qtKey) const;
    GhosttyMods mapQtModifiers(Qt::KeyboardModifiers mods) const;
    const QFont &fontForStyle(const GhosttyStyle &style) const;
    void updateFontMetrics();
    QPointF cellFromPixel(const QPointF &pos) const;
    void clearSelection();
    void drawSelectionHighlight(QPainter *painter, qreal offsetX, qreal offsetY, qreal scale);

    GhosttyVt *m_vt = nullptr;
    PtyManager *m_pty = nullptr;
    QImage m_image;
    uint16_t m_cols = 0;
    uint16_t m_rows = 0;
    int m_cellWidth = 0;
    int m_cellHeight = 0;

    // Pre-cached font variants to avoid per-cell QFont allocation
    QFont m_font;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;
    int m_fontSize = 10;
    int m_stickyModifiers = 0;

    QString m_title;

    // Touch selection state
    bool m_selecting = false;
    bool m_magnifierVisible = false;
    QPointF m_selStart;   // pixel coordinates
    QPointF m_selEnd;     // pixel coordinates
    int m_longPressTimerId = 0;
    static const int LongPressTimeout = 300; // ms — faster activation for better UX

    // Selection magnifier (SailfishOS-style zoom bubble)
    void renderMagnifier(QPainter *painter);
    static const int MagnifierZoom = 2;       // 2x zoom
    static const int MagnifierWidth = 180;    // px
    static const int MagnifierHeight = 100;   // px
    static const int MagnifierOffset = 20;    // px above finger

    // Mouse tracking for TUI apps (tmux, neovim, htop)
    bool m_mouseTrackingActive = false;
    bool m_mouseButtonPressed = false;  // tracks any-button state for encoder
    QPointF m_touchStartPos;

    // Two-finger scroll for touch devices
    bool m_twoFingerScrolling = false;
    qreal m_twoFingerLastY = 0;

    qreal m_scrollAccumulator = 0;
    qreal m_touchScrollAccumulator = 0;

    // Cached cursor cell for block cursor rendering (avoids O(rows) lookup)
    struct CachedCursorCell {
        uint32_t graphemes[128];
        int graphemesLen = 0;
        GhosttyStyle style;
        QColor bgColor;
        bool valid = false;
    } m_cachedCursor;

    bool m_needsRender = true;
    int m_renderTimerId = 0;
    static const int RenderInterval = 16; // ~60fps batch coalescing

    // Cursor blinking
    int m_blinkTimerId = 0;
    bool m_cursorBlinkVisible = true;
    static const int BlinkInterval = 500; // ms
    static const int BlinkPauseMs = 1000; // ms — pause blinking after input
    QElapsedTimer m_lastInputTime;

    // Shell exit state
    bool m_shellExited = false;
    int m_shellExitCode = 0;
};

#endif // TERMINALVIEW_H
