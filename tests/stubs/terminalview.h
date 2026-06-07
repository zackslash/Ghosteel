#ifndef TERMINALVIEW_H
#define TERMINALVIEW_H

#include <QObject>
#include <QString>

// Lightweight stub replacing QQuickPaintedItem-based TerminalView for unit tests.
// Provides the same interface that SessionManager depends on, without requiring
// Qt Quick or the Sailfish SDK.

class TerminalView : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int stickyModifiers READ stickyModifiers WRITE setStickyModifiers NOTIFY stickyModifiersChanged)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectedTextChanged)

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

    Q_INVOKABLE void paste() {}
    Q_INVOKABLE void copySelection() {}
    Q_INVOKABLE void sendKey(int, int = 0) {}
    Q_INVOKABLE void restartShell() {}
    Q_INVOKABLE void setActive(bool) {}
    Q_INVOKABLE QString workingDirectory() const { return m_workingDirectory; }
    Q_INVOKABLE void setWorkingDirectory(const QString &dir) { m_workingDirectory = dir; }
    Q_INVOKABLE void setAutorunCommand(const QString &cmd) { m_autorunCommand = cmd; }
    Q_INVOKABLE QString autorunCommand() const { return m_autorunCommand; }
    Q_INVOKABLE void suppressNextKeyboardAutoShow() {}
    void cleanup() {}

    // Test helpers — allow tests to control the stub's state
    void setTitle(const QString &t) { m_title = t; Q_EMIT titleChanged(); }
    void setSelectedText(const QString &t) { m_selectedText = t; Q_EMIT selectedTextChanged(); }

Q_SIGNALS:
    void fontSizeChanged();
    void titleChanged();
    void stickyModifiersChanged();
    void terminalBell();
    void desktopNotification(const QString &summary, const QString &body);
    void selectedTextChanged();

private:
    int m_fontSize = 10;
    int m_stickyModifiers = 0;
    QString m_title;
    QString m_selectedText;
    QString m_workingDirectory;
    QString m_autorunCommand;
};

#endif // TERMINALVIEW_H
