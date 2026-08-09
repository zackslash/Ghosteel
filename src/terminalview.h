// NOTE: tests/stubs/terminalview.h shares this guard name for -include stubbing.
// If you rename this guard, update the stub to match.
#ifndef TERMINALVIEW_H
#define TERMINALVIEW_H

#include <QQuickItem>
#include <QFont>
#include <QElapsedTimer>
#include <QVector>
#include <QPointF>
#include <QTouchEvent>

#include "ghosttyvt.h"
#include "textutil.h"

class PtyManager;

class TerminalView : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int stickyModifiers READ stickyModifiers WRITE setStickyModifiers NOTIFY stickyModifiersChanged)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectedTextChanged)
    Q_PROPERTY(int searchMatchCount READ searchMatchCount NOTIFY searchMatchCountChanged)
    Q_PROPERTY(int currentMatchIndex READ currentMatchIndex NOTIFY currentMatchIndexChanged)
    Q_PROPERTY(int searchPanelHeight READ searchPanelHeight WRITE setSearchPanelHeight NOTIFY searchPanelHeightChanged)
    Q_PROPERTY(int topPadding READ topPadding WRITE setTopPadding NOTIFY topPaddingChanged)
    Q_PROPERTY(QColor selectionHighlightColor READ selectionHighlightColor WRITE setSelectionHighlightColor NOTIFY selectionHighlightColorChanged)
    Q_PROPERTY(QColor selectionHandleColor READ selectionHandleColor WRITE setSelectionHandleColor NOTIFY selectionHandleColorChanged)
    Q_PROPERTY(QColor selectionHandleBorderColor READ selectionHandleBorderColor WRITE setSelectionHandleBorderColor NOTIFY selectionHandleBorderColorChanged)
    Q_PROPERTY(QColor searchHighlightColor READ searchHighlightColor WRITE setSearchHighlightColor NOTIFY searchHighlightColorChanged)
    Q_PROPERTY(QColor searchCurrentColor READ searchCurrentColor WRITE setSearchCurrentColor NOTIFY searchCurrentColorChanged)
    Q_PROPERTY(QColor shellExitOverlayColor READ shellExitOverlayColor WRITE setShellExitOverlayColor NOTIFY shellExitOverlayColorChanged)
    Q_PROPERTY(QColor shellExitTextColor READ shellExitTextColor WRITE setShellExitTextColor NOTIFY shellExitTextColorChanged)
    Q_PROPERTY(QColor magnifierBorderColor READ magnifierBorderColor WRITE setMagnifierBorderColor NOTIFY magnifierBorderColorChanged)
    Q_PROPERTY(int pullDownZoneHeight READ pullDownZoneHeight WRITE setPullDownZoneHeight NOTIFY pullDownZoneHeightChanged)
    Q_PROPERTY(bool pinchAtDefault READ pinchAtDefault NOTIFY pinchAtDefaultChanged)
    Q_PROPERTY(bool sessionSwipeEnabled READ sessionSwipeEnabled WRITE setSessionSwipeEnabled NOTIFY sessionSwipeEnabledChanged)

public:
    // Scrollback search match
    struct SearchMatch { int row; int cellCol; int cellWidth; };

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
    int cellWidth() const { return m_cellWidth; }
    int cellHeight() const { return m_cellHeight; }

    // Height of the top-docked search panel (0 when closed). Consumed by
    // scrollToMatch() to exclude rows hidden behind it from the visible range.
    int searchPanelHeight() const { return m_searchPanelHeight; }
    void setSearchPanelHeight(int height);

    // Session-swipe gate. False when only one session exists so the classifier
    // never kills the long-press (text-selection) timer for a gesture QML
    // would reject anyway. Bound from QML via SessionManager.sessionCount.
    bool sessionSwipeEnabled() const { return m_sessionSwipeEnabled; }
    void setSessionSwipeEnabled(bool enabled) {
        if (m_sessionSwipeEnabled != enabled) {
            m_sessionSwipeEnabled = enabled;
            Q_EMIT sessionSwipeEnabledChanged();
        }
    }

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
    int pullDownZoneHeight() const { return m_pullDownZoneHeight; }
    void setPullDownZoneHeight(int height);
    bool pinchAtDefault() const { return m_pinchAtDefault; }

    Q_INVOKABLE void paste();         // Paste from system clipboard
    Q_INVOKABLE void copySelection(); // Copy terminal selection to clipboard
    Q_INVOKABLE void sendClipboardText(const QString &text, const QString &kind = "c");
    Q_INVOKABLE void sendKey(int qtKey, int modifiers = 0);
    Q_INVOKABLE void restartShell();  // Restart shell after exit
    Q_INVOKABLE QString workingDirectory() const; // Get CWD from /proc/<pid>/cwd
    Q_INVOKABLE void setWorkingDirectory(const QString &dir); // Set CWD for next shell start
    Q_INVOKABLE void setAutorunCommand(const QString &cmd);
    void setCommandArgs(const QStringList &args);
    Q_INVOKABLE void suppressNextKeyboardAutoShow();
    void setPendingScrollback(const QByteArray &data); // Set VT data for restore on setupTerminal()
    Q_INVOKABLE void openSearch();
    Q_INVOKABLE void closeSearch();
    Q_INVOKABLE void setSearchPattern(const QString &pattern);
    Q_INVOKABLE void findNext();
    Q_INVOKABLE void findPrevious();
    void cleanup();                   // Stop PTY/threads before destruction

    // Link detection — exposed for GLRenderer to trigger scans in synchronize()
    void refreshLinks();               // Run regex scan on visible viewport
    bool isLinkScanDirty() const { return m_linkScanDirty; }

    // Expose GhosttyVt for GL renderer access
    GhosttyVt *vt() const { return m_vt; }

    // Expose cursor blink state for GL renderer (single source of truth)
    bool cursorBlinkVisible() const { return m_cursorBlinkVisible; }

    // Shared constants for selection handles and magnifier
    static const int HandleRadius = 14; // px — touch-friendly target size
    static const int MagnifierZoom = 2;
    static const int MagnifierWidth = 180;  // px
    static const int MagnifierHeight = 100; // px
    static const int MagnifierOffset = 20;  // px above finger

    // Overlay state getters for GLRenderer
    bool isSelecting() const { return m_selecting; }
    QPointF selectionStart() const { return m_selStart; }
    QPointF selectionEnd() const { return m_selEnd; }
    bool handlesVisible() const { return m_handlesVisible; }
    const QVector<SearchMatch>& searchMatches() const { return m_searchMatches; }
    bool shellExited() const { return m_shellExited; }
    int shellExitCode() const { return m_shellExitCode; }
    bool magnifierVisible() const { return m_magnifierVisible; }
    int draggingHandle() const { return m_draggingHandle; }
    const QVector<TextUtil::LinkSpan>& currentLinks() const { return m_currentLinks; }

    // Scrollback persistence — wraps GhosttyVt export for SessionManager access
    QByteArray exportScrollback(uint16_t &outCols, uint16_t &outRows) const;

Q_SIGNALS:
    void fontSizeChanged();
    void titleChanged();
    void stickyModifiersChanged();
    void terminalBell();
    void desktopNotification(const QString &summary, const QString &body);
    void clipboardReadRequest(const QString &kind);
    void clipboardTextReady(const QString &text);
    void selectedTextChanged();
    void linkActivated(const QString &uri);
    void searchMatchCountChanged();
    void currentMatchIndexChanged();
    void searchPanelHeightChanged();
    void selectionHighlightColorChanged();
    void selectionHandleColorChanged();
    void selectionHandleBorderColorChanged();
    void searchHighlightColorChanged();
    void searchCurrentColorChanged();
    void shellExitOverlayColorChanged();
    void shellExitTextColorChanged();
    void magnifierBorderColorChanged();
    void pullDownZoneHeightChanged();
    void navigateSession(int direction);
    void toggleKeybar();
    void commandExited(int exitCode);
    void shellRestarted();
    void topPaddingChanged();
    void contentChanged(); // Emitted on real content change (PTY data) for scrollback tracking
    void repaintRequested(); // Emitted on every repaint request for GL renderer
    void ptyDataReceived(); // Emitted when real PTY data arrives (before vtWrite)
    void pinchingChanged(bool pinching);
    void pinchAtDefaultChanged(bool atDefault);
    void zoomRequested(int delta);       // +1 for zoom in, -1 for zoom out
    // Toggle parent SilicaFlickable.interactive — emitted false on
    // multi-touch/TUI begin, true on end.
    void requestParentInteractive(bool interactive);

    // Session-swipe gesture (horizontal one-finger drag -> switch session).
    // QML drives the slide animation + switchSession() from these. Guarded:
    // only emitted in NORMAL single-finger mode, pre-selection window, never
    // during TUI/multitouch/handle/link.
    void sessionSwipeStarted();
    void sessionSwipeProgress(qreal deltaX);   // signed px from swipeStartX
    void sessionSwipeCommitted(int direction); // +1 = next, -1 = previous
    void sessionSwipeCancelled();
    void sessionSwipeEnabledChanged();

protected:
    void update(); // Override to emit repaintRequested() for GL repaint
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
    void sendKeyEvent(GhosttyKey key, GhosttyKeyAction action,
                      GhosttyMods mods, const QString &text);
    void sendMouseEvent(GhosttyMouseAction action, GhosttyMouseButton button,
                        const QPointF &pos, GhosttyMods mods);
    void resetBlinkOnInput();
    void runAutorunCommand();

    void updateFontMetrics();
    QPointF cellFromPixel(const QPointF &pos) const;
    QPointF cellFromPixelClamped(const QPointF &pos) const;
    void clearSelection();
    void selectWordAt(const QPointF &pos);
    void selectLineAt(const QPointF &pos);
    int handleHitTest(const QPointF &pos) const; // 0=none, 1=start, 2=end

    void performSearch();
    void scrollToMatch(int index);
    // Re-extract the search cache after the terminal changed (resize or live
    // PTY output), then re-run performSearch() keeping m_currentMatchIndex on
    // the match nearest to the previously-current match's row.
    void refreshSearchCachePreservingMatch();
    void scrollViewportToBottom();
    void resetSessionSwipe(); // defensive — call from every path that abandons a gesture
    void resetTouchInteractionState(); // consolidate TouchCancel / release state resets

    // --- Pinch-to-zoom gesture disambiguation ---
    void handleMultiTouchBegin(const QList<QTouchEvent::TouchPoint> &points);
    void handleMultiTouchUpdate(const QList<QTouchEvent::TouchPoint> &points);
    void handleMultiTouchEnd();

    // TUI single-finger touch -> synthetic mouse/wheel events
    void handleTuiTouchBegin(QTouchEvent *event, const QTouchEvent::TouchPoint &pt);
    void handleTuiTouchUpdate(QTouchEvent *event, const QTouchEvent::TouchPoint &pt);
    void handleTuiTouchEnd(QTouchEvent *event, const QList<QTouchEvent::TouchPoint> &points);

    // --- Core terminal state ---
    GhosttyVt *m_vt = nullptr;
    PtyManager *m_pty = nullptr;
    uint16_t m_cols = 0;
    uint16_t m_rows = 0;
    int m_cellWidth = 0;
    int m_cellHeight = 0;

    // --- Font ---
    QFont m_font;
    int m_fontSize = 18;
    int m_stickyModifiers = 0;

    QString m_title;

    // --- Touch text selection (long-press -> drag -> copy) ---
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

    // Selected text (exposed to QML for share action)
    QString m_selectedText;

    // Selection handles (SailfishOS-style draggable endpoints)
    bool m_handlesVisible = false;
    int m_draggingHandle = 0; // 0=none, 1=start, 2=end

    // Selection magnifier (SailfishOS-style zoom bubble)
    // Constants are public — see HandleRadius, MagnifierZoom, etc. above

    // --- Mouse tracking for TUI apps (tmux, neovim, htop) ---
    bool m_mouseTrackingActive = false;
    bool m_mouseButtonPressed = false;  // tracks any-button state for encoder

    qreal m_tuiScrollAccumulator = 0;
    qreal m_tuiDragLastY = 0;

    // --- Scroll state (two-finger touch + mouse wheel) ---
    qreal m_twoFingerLastY = 0;
    qreal m_scrollAccumulator = 0;
    qreal m_touchScrollAccumulator = 0;

    // True between handleMultiTouchBegin/End. Needed because Qt delivers
    // TouchUpdate (not TouchBegin) when the second finger lands after the
    // first — so we start the gesture on the first ≥2-point event of any type.
    bool m_multiTouchActive = false;

    // --- Pinch-to-zoom state ---
    // Touch state machine:
    //   Idle -> [≥2 fingers] -> MultiTouch (Undecided -> Scrolling | Pinching)
    //   MultiTouch -> [all fingers up or drop below 2] -> Idle
    //   TUI mode: single-finger touches are grabbed and forwarded as synthetic mouse events
    //   Normal mode: single-finger touches fall through to QQuickItem/Flickable
    enum class GestureMode { Undecided, Scrolling, Pinching };
    GestureMode m_gestureMode = GestureMode::Undecided;
    qreal m_pinchInitialDistance = 0;
    int m_pinchBaseFontSize = 0;
    int m_lastAppliedFontSize = 0;
    int m_pinchCandidateFrames = 0;
    bool m_pinchAtDefault = false;
    QPointF m_gestureInitialCentroid;

    static constexpr qreal PinchRatioThreshold = 1.12;
    static constexpr int PinchRatioFrames = 2;
    static constexpr qreal ScrollMinDistancePx = 40.0;
    static constexpr qreal PinchScaleExponent = 0.6; // <1 dampens; 0.5=sqrt, 1.0=linear

    // --- Session-swipe gesture (horizontal one-finger drag -> switch session) ---
    bool    m_sessionSwiping = false;
    bool    m_sessionSwipeEnabled = true; // QML-bound gate (false for single session)
    qreal   m_swipeStartX    = 0.0;   // captured at classify time, not at press
    static constexpr qreal SwipeMinHorizontalPx = 24.0; // < ScrollMinDistancePx (40)
    static constexpr qreal SwipeDominanceRatio  = 1.5;  // |dx| must exceed |dy| * this
    static constexpr qreal SwipeCommitFraction  = 0.25; // release past 25% width -> commit

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
    QStringList m_commandArgs;  // If non-empty, startCommand() is used instead of startShell()
    static const int AutorunDelayMs = 500;

    // --- Suppress keyboard auto-show flag ---
    bool m_suppressKeyboardAutoShow = false;

    // --- Pending scrollback data for restore ---
    QByteArray m_pendingScrollback;

    // --- Scrollback search ---
    bool m_searchActive = false;
    QString m_searchPattern;
    QStringList m_searchCache;       // Cached terminal text (one string per row)
    QVector<QVector<int>> m_cellMapping; // Per row: cell index -> character index offset
    QVector<SearchMatch> m_searchMatches;
    int m_currentMatchIndex = -1;
    int m_searchPanelHeight = 0;

    // --- Link detection (OSC 8 hyperlinks + regex URL scanning) ---
    QVector<TextUtil::LinkSpan> m_currentLinks;  // Cached regex-detected links for viewport
    bool m_linkScanDirty = true;       // Set when PTY data arrives, cleared after scan
    QElapsedTimer m_lastLinkScanTime; // Throttle: limit link scans to ~4Hz
    bool m_pendingLinkTap = false;     // True between press and release on a link
    QString m_tappedLinkUri;           // URI of the tapped link
    QPointF m_linkTapStartPos;         // Position where link tap started
    QString findRegexLinkAt(int col, int row) const; // Returns URI string
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
    int m_pullDownZoneHeight = 100; // px — overridden from QML via Theme.itemSizeLarge
};

#endif // TERMINALVIEW_H
