#ifndef SETTINGS_H
#define SETTINGS_H

#include <QObject>
#include <QSettings>
#include <QTimer>

// Singleton settings manager backed by QSettings (INI format).
// Config file: ~/.config/<APP_ORG>/<APP_NAME>.conf
// Thread safety: Must only be accessed from the main GUI thread.
// QSettings is not thread-safe for concurrent reads/writes.
class Settings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)
    Q_PROPERTY(QString shellCommand READ shellCommand WRITE setShellCommand NOTIFY shellCommandChanged)
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
    Q_PROPERTY(float backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY backgroundOpacityChanged)
    Q_PROPERTY(int bellMode READ bellMode WRITE setBellMode NOTIFY bellModeChanged)
    Q_PROPERTY(bool scrollbackPersistence READ scrollbackPersistence WRITE setScrollbackPersistence NOTIFY scrollbackPersistenceChanged)
    Q_PROPERTY(int scrollbackRetentionDays READ scrollbackRetentionDays WRITE setScrollbackRetentionDays NOTIFY scrollbackRetentionDaysChanged)

public:
    static Settings *instance();

    // Test constructor: allows injecting a custom settings path
    explicit Settings(const QString &settingsPath, QObject *parent = nullptr);

    int fontSize() const { return m_fontSize; }
    void setFontSize(int size);

    QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString &family);

    QString shellCommand() const { return m_shellCommand; }
    void setShellCommand(const QString &cmd);

    QString colorScheme() const { return m_colorScheme; }
    void setColorScheme(const QString &scheme);

    float backgroundOpacity() const { return m_backgroundOpacity; }
    void setBackgroundOpacity(float opacity);

    int bellMode() const { return m_bellMode; }
    void setBellMode(int mode);

    bool scrollbackPersistence() const { return m_scrollbackPersistence; }
    void setScrollbackPersistence(bool enabled);

    int scrollbackRetentionDays() const { return m_scrollbackRetentionDays; }
    void setScrollbackRetentionDays(int days);

Q_SIGNALS:
    void fontSizeChanged();
    void fontFamilyChanged();
    void shellCommandChanged();
    void colorSchemeChanged();
    void backgroundOpacityChanged();
    void bellModeChanged();
    void scrollbackPersistenceChanged();
    void scrollbackRetentionDaysChanged();

private:
    explicit Settings(QObject *parent = nullptr);
    void load();
    void save();
    void scheduleSave();

    QSettings m_settings;
    QTimer *m_saveTimer;
    int m_fontSize = 18;
    QString m_fontFamily = QStringLiteral("DejaVu Sans Mono");
    QString m_shellCommand;
    QString m_colorScheme = QStringLiteral("dark");
    float m_backgroundOpacity = 0.6f;
    int m_bellMode = 1; // default: Vibrate
    bool m_scrollbackPersistence = false; // default: off (opt-in)
    int m_scrollbackRetentionDays = 30;   // default: 30 days
};

#endif // SETTINGS_H
