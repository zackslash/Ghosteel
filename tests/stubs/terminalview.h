#ifndef TERMINALVIEW_H
#define TERMINALVIEW_H

#include <QObject>
#include <QString>
#include <QColor>

// Lightweight stub replacing QQuickItem-based TerminalView for unit tests.
// Provides the same interface that SessionManager depends on, without requiring
// Qt Quick or the Sailfish SDK.

class TerminalView : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int stickyModifiers READ stickyModifiers WRITE setStickyModifiers NOTIFY stickyModifiersChanged)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectedTextChanged)
    Q_PROPERTY(int searchMatchCount READ searchMatchCount NOTIFY searchMatchCountChanged)
    Q_PROPERTY(int currentMatchIndex READ currentMatchIndex NOTIFY currentMatchIndexChanged)
    Q_PROPERTY(bool searchActive READ searchActive NOTIFY searchActiveChanged)
    Q_PROPERTY(QColor selectionHighlightColor READ selectionHighlightColor WRITE setSelectionHighlightColor NOTIFY selectionHighlightColorChanged)
    Q_PROPERTY(QColor selectionHandleColor READ selectionHandleColor WRITE setSelectionHandleColor NOTIFY selectionHandleColorChanged)
    Q_PROPERTY(QColor selectionHandleBorderColor READ selectionHandleBorderColor WRITE setSelectionHandleBorderColor NOTIFY selectionHandleBorderColorChanged)
    Q_PROPERTY(QColor searchHighlightColor READ searchHighlightColor WRITE setSearchHighlightColor NOTIFY searchHighlightColorChanged)
    Q_PROPERTY(QColor searchCurrentColor READ searchCurrentColor WRITE setSearchCurrentColor NOTIFY searchCurrentColorChanged)
    Q_PROPERTY(QColor shellExitOverlayColor READ shellExitOverlayColor WRITE setShellExitOverlayColor NOTIFY shellExitOverlayColorChanged)
    Q_PROPERTY(QColor shellExitTextColor READ shellExitTextColor WRITE setShellExitTextColor NOTIFY shellExitTextColorChanged)
    Q_PROPERTY(QColor magnifierBorderColor READ magnifierBorderColor WRITE setMagnifierBorderColor NOTIFY magnifierBorderColorChanged)
    Q_PROPERTY(int topPadding READ topPadding WRITE setTopPadding NOTIFY topPaddingChanged)
    Q_PROPERTY(int pullDownZoneHeight READ pullDownZoneHeight WRITE setPullDownZoneHeight NOTIFY pullDownZoneHeightChanged)

public:
    explicit TerminalView(QObject *parent = nullptr) : QObject(parent) {}
    ~TerminalView() override = default;

    int fontSize() const { return m_fontSize; }
    void setFontSize(int size) {
        if (m_fontSize != size) { m_fontSize = size; Q_EMIT fontSizeChanged(); }
    }
    QString title() const { return m_title; }
    int stickyModifiers() const { return m_stickyModifiers; }
    void setStickyModifiers(int mods) {
        if (m_stickyModifiers != mods) { m_stickyModifiers = mods; Q_EMIT stickyModifiersChanged(); }
    }
    QString selectedText() const { return m_selectedText; }
    int searchMatchCount() const { return 0; }
    int currentMatchIndex() const { return m_currentMatchIndex; }
    bool searchActive() const { return m_searchActive; }

    QColor selectionHighlightColor() const { return m_selectionHighlightColor; }
    void setSelectionHighlightColor(const QColor &color) {
        if (m_selectionHighlightColor != color) { m_selectionHighlightColor = color; Q_EMIT selectionHighlightColorChanged(); }
    }
    QColor selectionHandleColor() const { return m_selectionHandleColor; }
    void setSelectionHandleColor(const QColor &color) {
        if (m_selectionHandleColor != color) { m_selectionHandleColor = color; Q_EMIT selectionHandleColorChanged(); }
    }
    QColor selectionHandleBorderColor() const { return m_selectionHandleBorderColor; }
    void setSelectionHandleBorderColor(const QColor &color) {
        if (m_selectionHandleBorderColor != color) { m_selectionHandleBorderColor = color; Q_EMIT selectionHandleBorderColorChanged(); }
    }
    QColor searchHighlightColor() const { return m_searchHighlightColor; }
    void setSearchHighlightColor(const QColor &color) {
        if (m_searchHighlightColor != color) { m_searchHighlightColor = color; Q_EMIT searchHighlightColorChanged(); }
    }
    QColor searchCurrentColor() const { return m_searchCurrentColor; }
    void setSearchCurrentColor(const QColor &color) {
        if (m_searchCurrentColor != color) { m_searchCurrentColor = color; Q_EMIT searchCurrentColorChanged(); }
    }
    QColor shellExitOverlayColor() const { return m_shellExitOverlayColor; }
    void setShellExitOverlayColor(const QColor &color) {
        if (m_shellExitOverlayColor != color) { m_shellExitOverlayColor = color; Q_EMIT shellExitOverlayColorChanged(); }
    }
    QColor shellExitTextColor() const { return m_shellExitTextColor; }
    void setShellExitTextColor(const QColor &color) {
        if (m_shellExitTextColor != color) { m_shellExitTextColor = color; Q_EMIT shellExitTextColorChanged(); }
    }
    QColor magnifierBorderColor() const { return m_magnifierBorderColor; }
    void setMagnifierBorderColor(const QColor &color) {
        if (m_magnifierBorderColor != color) { m_magnifierBorderColor = color; Q_EMIT magnifierBorderColorChanged(); }
    }
    int topPadding() const { return m_topPadding; }
    void setTopPadding(int padding) {
        if (m_topPadding != padding) { m_topPadding = padding; Q_EMIT topPaddingChanged(); }
    }
    int pullDownZoneHeight() const { return m_pullDownZoneHeight; }
    void setPullDownZoneHeight(int height) {
        if (m_pullDownZoneHeight != height) { m_pullDownZoneHeight = height; Q_EMIT pullDownZoneHeightChanged(); }
    }

    Q_INVOKABLE void paste() {}
    Q_INVOKABLE void copySelection() {}
    Q_INVOKABLE void sendClipboardText(const QString &, const QString & = "c") {}
    Q_INVOKABLE void sendKey(int, int = 0) {}
    Q_INVOKABLE void restartShell() {}
    Q_INVOKABLE void setActive(bool) {}
    Q_INVOKABLE QString workingDirectory() const { return m_workingDirectory; }
    Q_INVOKABLE void setWorkingDirectory(const QString &dir) { m_workingDirectory = dir; }
    Q_INVOKABLE void setAutorunCommand(const QString &cmd) { m_autorunCommand = cmd; }
    Q_INVOKABLE QString autorunCommand() const { return m_autorunCommand; }
    Q_INVOKABLE void suppressNextKeyboardAutoShow() {}
    Q_INVOKABLE void setPendingScrollback(const QByteArray &) {}
    Q_INVOKABLE void openSearch() { m_searchActive = true; Q_EMIT searchActiveChanged(); }
    Q_INVOKABLE void closeSearch() { m_searchActive = false; m_currentMatchIndex = -1; Q_EMIT searchActiveChanged(); Q_EMIT searchMatchCountChanged(); Q_EMIT currentMatchIndexChanged(); }
    Q_INVOKABLE void setSearchPattern(const QString &) {}
    Q_INVOKABLE void findNext() {}
    Q_INVOKABLE void findPrevious() {}
    void cleanup() {}

    // Stub for scrollback persistence (GhosttyVt not available in stubs)
    QByteArray exportScrollback(uint16_t &, uint16_t &) const { return {}; }

    // Test helpers — allow tests to control the stub's state
    void setTitle(const QString &t) { m_title = t; Q_EMIT titleChanged(); }
    void setSelectedText(const QString &t) { m_selectedText = t; Q_EMIT selectedTextChanged(); }

Q_SIGNALS:
    void fontSizeChanged();
    void titleChanged();
    void stickyModifiersChanged();
    void terminalBell();
    void desktopNotification(const QString &summary, const QString &body);
    void clipboardReadRequest(const QString &kind);
    void clipboardTextReady(const QString &text);
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
    void pullDownZoneHeightChanged();
    void navigateSession(int direction);
    void toggleKeybar();
    void linkActivated(const QString &uri);
    void topPaddingChanged();
    void pinchingChanged(bool pinching);
    void zoomRequested(int delta);
    void requestParentInteractive(bool interactive);

private:
    int m_fontSize = 18;
    int m_stickyModifiers = 0;
    QString m_title;
    QString m_selectedText;
    QString m_workingDirectory;
    QString m_autorunCommand;

    int m_currentMatchIndex = -1;
    bool m_searchActive = false;

    QColor m_selectionHighlightColor = QColor(255, 255, 255, 76);
    QColor m_selectionHandleColor = QColor(255, 255, 255, 200);
    QColor m_selectionHandleBorderColor = QColor(255, 255, 255, 120);
    QColor m_searchHighlightColor = QColor(255, 200, 0, 100);
    QColor m_searchCurrentColor = QColor(255, 100, 0, 140);
    QColor m_shellExitOverlayColor = QColor(0, 0, 0, 180);
    QColor m_shellExitTextColor = Qt::white;
    QColor m_magnifierBorderColor = QColor(255, 255, 255, 120);
    int m_topPadding = 12;
    int m_pullDownZoneHeight = 100;
};

#endif // TERMINALVIEW_H
