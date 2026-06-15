// NOTE: tests/stubs/terminalview.h shares this guard name for -include stubbing.
// If you rename this guard, update the stub to match.
#ifndef TERMINALVIEW_H
#define TERMINALVIEW_H

#include <QQuickPaintedItem>
#include <QImage>
#include <QFont>
#include <QElapsedTimer>
#include <QVector>

#include "ghosttyvt.h"
#include "textutil.h"

class PtyManager;

class TerminalView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int stickyModifiers READ stickyModifiers WRITE setStickyModifiers NOTIFY stickyModifiersChanged)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectedTextChanged)
    Q_PROPERTY(int searchMatchCount READ searchMatchCount NOTIFY searchMatchCountChanged)
    Q_PROPERTY(int currentMatchIndex READ currentMatchIndex NOTIFY currentMatchIndexChanged)
    Q_PROPERTY(bool searchActive READ searchActive NOTIFY searchActiveChanged)
    Q_PROPERTY(int topPadding READ topPadding WRITE setTopPadding NOTIFY topPaddingChanged)
    Q_PROPERTY(QColor selectionHighlightColor READ selectionHighlightColor WRITE setSelectionHighlightColor NOTIFY selectionHighlightColorChanged)
    Q_PROPERTY(QColor selectionHandleColor READ selectionHandleColor WRITE setSelectionHandleColor NOTIFY selectionHandleColorChanged)
    Q_PROPERTY(QColor selectionHandleBorderColor READ selectionHandleBorderColor WRITE setSelectionHandleBorderColor NOTIFY selectionHandleBorderColorChanged)
    Q_PROPERTY(QColor searchHighlightColor READ searchHighlightColor WRITE setSearchHighlightColor NOTIFY searchHighlightColorChanged)
    Q_PROPERTY(QColor searchCurrentColor READ searchCurrentColor WRITE setSearchCurrentColor NOTIFY searchCurrentColorChanged)
    Q_PROPERTY(QColor shellExitOverlayColor READ shellExitOverlayColor WRITE setShellExitOverlayColor NOTIFY shellExitOverlayColorChanged)
    Q_PROPERTY(QColor shellExitTextColor READ shellExitTextColor WRITE setShellExitTextColor NOTIFY shellExitTextColorChanged)
    Q_PROPERTY(QColor magnifierBorderColor READ magnifierBorderColor WRITE setMagnifierBorderColor NOTIFY magnifierBorderColorChanged)
public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView();

    int fontSize() const { return m_fontSize; }
    void setFontSize(int size);
    QString title() const { return m_title; }
    int stickyModifiers() const { return m_stickyModifiers; }
    void setStickyModifiers(int mods);
    QString selectedText() const { return m_selectedText; }
    int searchMatchCount() const { return m_searchMatches.size(); }
    int currentMatchIndex() const { return m_currentMatchIndex; }
    bool searchActive() const { return m_searchActive; }
    int topPadding() const { return m_topPadding; }
    void setTopPadding(int padding);

    QColor selectionHighlightColor() const { return m_selectionHighlightColor; }
    void setSelectionHighlightColor(const QColor &color);
    QColor selectionHandleColor() const { return m_selectionHandleColor; }
    void setSelectionHandleColor(const QColor &color);
    QColor selectionHandleBorderColor() const { return m_selectionHandleBorderColor; }
    void setSelectionHandleBorderColor(const QColor &color);
    QColor searchHighlightColor() const { return m_searchHighlightColor; }
    void setSearchHighlightColor(const QColor &color);
    QColor searchCurrentColor() const { return m_searchCurrentColor; }
    void setSearchCurrentColor(const QColor &color);
    QColor shellExitOverlayColor() const { return m_shellExitOverlayColor; }
    void setShellExitOverlayColor(const QColor &color);
    QColor shellExitTextColor() const { return m_shellExitTextColor; }
    void setShellExitTextColor(const QColor &color);
    QColor magnifierBorderColor() const { return m_magnifierBorderColor; }
    void setMagnifierBorderColor(const QColor &color);

    Q_INVOKABLE void paste();         // Paste from system clipboard
    Q_INVOKABLE void copySelection(); // Copy terminal selection to clipboard
    Q_INVOKABLE void sendKey(int qtKey, int modifiers = 0);
    Q_INVOKABLE void restartShell();  // Restart shell after exit
    Q_INVOKABLE void setActive(bool active); // Start/stop blink timer
    Q_INVOKABLE QString workingDirectory() const; // Get CWD from /proc/<pid>/cwd
    Q_INVOKABLE void setWorkingDirectory(const QString &dir); // Set CWD for next shell start
    Q_INVOKABLE void setAutorunCommand(const QString &cmd);
    Q_INVOKABLE void suppressNextKeyboardAutoShow();
    void setPendingScrollback(const QByteArray &data); // Set VT data for restore on setupTerminal()
    Q_INVOKABLE void openSearch();
    Q_INVOKABLE void closeSearch();
    Q_INVOKABLE void setSearchPattern(const QString &pattern);
    Q_INVOKABLE void findNext();
    Q_INVOKABLE void findPrevious();
    void cleanup();                   // Stop PTY/threads before destruction

    // Scrollback persistence — wraps GhosttyVt export for SessionManager access
    QByteArray exportScrollback(uint16_t &outCols, uint16_t &outRows) const;

Q_SIGNALS:
    void fontSizeChanged();
    void titleChanged();
    void stickyModifiersChanged();
    void terminalBell();
    void desktopNotification(const QString &summary, const QString &body);
    void selectedTextChanged();
    void searchMatchCountChanged();
    void currentMatchIndexChanged();
    void searchActiveChanged();
    void selectionHighlightColorChanged();
    void selectionHandleColorChanged();
    void selectionHandleBorderColorChanged();
    void searchHighlightColorChanged();
    void searchCurrentColorChanged();
    void shellExitOverlayColorChanged();
    void shellExitTextColorChanged();
    void magnifierBorderColorChanged();
    void navigateSession(int direction);
    void toggleKeybar();
    void topPaddingChanged();

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
    void sendMouseEvent(GhosttyMouseAction action, GhosttyMouseButton button,
                        const QPointF &pos, GhosttyMods mods);
    void resetBlinkOnInput();
    void runAutorunCommand();
    const QFont &fontForStyle(const GhosttyStyle &style) const;

    // Cell data extracted from the render state for a single grid position.
    // Used by drawBlockCursorText() to avoid duplicating the cell-reading logic.
    struct CellData {
        uint32_t graphemes[128];
        uint32_t graphemesLen = 0;
        GhosttyStyle style;
        QColor bgColor;
        bool valid = false;
    };
    bool readCellAt(GhosttyRenderState state, int col, int row,
                    const QColor &defaultBg, CellData &out) const;
    void drawBlockCursorText(QPainter *painter, int px, int py,
                             GhosttyRenderState state, const QColor &bgColor,
                             const QColor &fgColor);
    void drawCursor(QPainter *painter, GhosttyRenderState state,
                    const GhosttyRenderStateColors &colors,
                    const QColor &fgColor);
    void renderCellGrid(QPainter *painter, GhosttyRenderState state,
                        const QColor &bgColor, const QColor &fgColor);
    void drawShellExitOverlay();
    void updateFontMetrics();
    QPointF cellFromPixel(const QPointF &pos) const;
    void clearSelection();
    void drawSelectionHighlight(QPainter *painter, qreal offsetX, qreal offsetY, qreal scale);
    void selectWordAt(const QPointF &pos);
    void selectLineAt(const QPointF &pos);
    void drawSelectionHandles(QPainter *painter);
    int handleHitTest(const QPointF &pos) const; // 0=none, 1=start, 2=end
    bool updateMagnifierVelocity(const QPointF &pos); // returns true if magnifier should be visible
    void performSearch();
    void scrollToMatch(int index);
    void drawSearchHighlights(QPainter *painter);
    void buildCellMapping();
    void scrollViewportToBottom();

    // --- Core terminal state ---
    GhosttyVt *m_vt = nullptr;
    PtyManager *m_pty = nullptr;
    QImage m_image;
    uint16_t m_cols = 0;
    uint16_t m_rows = 0;
    int m_cellWidth = 0;
    int m_cellHeight = 0;

    // --- Font (pre-cached variants to avoid per-cell QFont allocation) ---
    QFont m_font;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;
    int m_fontSize = 10;
    int m_stickyModifiers = 0;

    QString m_title;

    // --- Touch text selection (long-press → drag → copy) ---
    bool m_selecting = false;
    bool m_magnifierVisible = false;
    QPointF m_selStart;   // pixel coordinates
    QPointF m_selEnd;     // pixel coordinates
    int m_longPressTimerId = 0;
    static const int LongPressTimeout = 300; // ms — faster activation for better UX

    // Tap detection for double/triple tap word/line selection
    qint64 m_lastTapTime = 0;     // ms since epoch
    QPointF m_lastTapPos;
    int m_tapCount = 0;            // 1=single, 2=double, 3=triple
    static const int TapTimeoutMs = 300;   // ms between taps for double/triple
    static const int TapDistancePx = 30;   // max pixel distance between taps

    // Velocity tracking for magnifier hiding during fast swipes
    qint64 m_lastMoveTime = 0;
    QPointF m_lastMovePos;
    bool m_velocityInitialized = false;
    // Hysteresis thresholds prevent flicker when velocity oscillates around the boundary
    static const int MagnifierVelocityHide = 600; // px/s — hide above this
    static const int MagnifierVelocityShow = 400;  // px/s — show below this

    // Selected text (exposed to QML for share action)
    QString m_selectedText;

    // Selection handles (SailfishOS-style draggable endpoints)
    bool m_handlesVisible = false;
    int m_draggingHandle = 0; // 0=none, 1=start, 2=end
    static const int HandleRadius = 14; // px — touch-friendly target size

    // Selection magnifier (SailfishOS-style zoom bubble)
    void renderMagnifier(QPainter *painter);
    static const int MagnifierZoom = 2;       // 2x zoom
    static const int MagnifierWidth = 180;    // px
    static const int MagnifierHeight = 100;   // px
    static const int MagnifierOffset = 20;    // px above finger

    // --- Mouse tracking for TUI apps (tmux, neovim, htop) ---
    bool m_mouseTrackingActive = false;
    bool m_mouseButtonPressed = false;  // tracks any-button state for encoder
    QPointF m_touchStartPos;

    // --- Scroll state (two-finger touch + mouse wheel) ---
    bool m_twoFingerScrolling = false;
    qreal m_twoFingerLastY = 0;
    qreal m_scrollAccumulator = 0;
    qreal m_touchScrollAccumulator = 0;

    // Cached cursor cell for block cursor rendering (avoids O(rows) fallback)
    CellData m_cachedCursor;

    // --- Render batching (~60fps coalescing for rapid PTY data) ---
    bool m_needsRender = true;
    int m_renderTimerId = 0;
    static const int RenderInterval = 16; // ms

    // --- Cursor blinking (pauses after input for 1s) ---
    int m_blinkTimerId = 0;
    bool m_cursorBlinkVisible = true;
    static const int BlinkInterval = 500; // ms
    static const int BlinkPauseMs = 1000; // ms — pause after input
    QElapsedTimer m_lastInputTime;

    // --- Shell exit state ---
    bool m_shellExited = false;
    int m_shellExitCode = 0;

    // --- Autorun command (per-session startup command) ---
    QString m_autorunCommand;
    static const int AutorunDelayMs = 500;

    // --- Suppress keyboard auto-show flag ---
    bool m_suppressKeyboardAutoShow = false;

    // --- Pending scrollback data for restore ---
    QByteArray m_pendingScrollback;

    // --- Scrollback search ---
    struct SearchMatch { int row; int cellCol; int cellWidth; };
    bool m_searchActive = false;
    QString m_searchPattern;
    QStringList m_searchCache;       // Cached terminal text (one string per row)
    QVector<QVector<int>> m_cellMapping; // Per row: cell index → character index offset
    QVector<SearchMatch> m_searchMatches;
    int m_currentMatchIndex = -1;

    // --- Link detection (OSC 8 hyperlinks + regex URL scanning) ---
    QVector<TextUtil::LinkSpan> m_currentLinks;  // Cached regex-detected links for viewport
    bool m_linkScanDirty = true;       // Set when PTY data arrives, cleared after scan
    bool m_pendingLinkTap = false;     // True between press and release on a link
    QString m_tappedLinkUri;           // URI of the tapped link
    QPointF m_linkTapStartPos;         // Position where link tap started
    void refreshLinks();               // Run regex scan on visible viewport
    QString findRegexLinkAt(int col, int row) const; // Check cached regex links
    QString findLinkAt(int col, int row); // Check OSC 8 first, then regex

    // --- Theme-bindable UI colors ---
    QColor m_selectionHighlightColor = QColor(255, 255, 255, 76);
    QColor m_selectionHandleColor = QColor(255, 255, 255, 200);
    QColor m_selectionHandleBorderColor = QColor(255, 255, 255, 120);
    QColor m_searchHighlightColor = QColor(255, 200, 0, 100);
    QColor m_searchCurrentColor = QColor(255, 100, 0, 140);
    QColor m_shellExitOverlayColor = QColor(0, 0, 0, 180);
    QColor m_shellExitTextColor = Qt::white;
    QColor m_magnifierBorderColor = QColor(255, 255, 255, 120);
    int m_topPadding = 12; // default matches original static const
};

#endif // TERMINALVIEW_H
