#ifndef SETTINGS_H
#define SETTINGS_H

#include <QObject>
#include <QSettings>
#include <QStringList>
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
    Q_PROPERTY(QStringList keybarKeys READ keybarKeys WRITE setKeybarKeys NOTIFY keybarKeysChanged)
    Q_PROPERTY(bool keybarVisible READ keybarVisible WRITE setKeybarVisible NOTIFY keybarVisibleChanged)
    Q_PROPERTY(int sessionSortMode READ sessionSortMode WRITE setSessionSortMode NOTIFY sessionSortModeChanged)
    Q_PROPERTY(bool cursorTrails READ cursorTrails WRITE setCursorTrails NOTIFY cursorTrailsChanged)
    Q_PROPERTY(bool pinchToZoom READ pinchToZoom WRITE setPinchToZoom NOTIFY pinchToZoomChanged)
    Q_PROPERTY(bool urlAutoDetect READ urlAutoDetect WRITE setUrlAutoDetect NOTIFY urlAutoDetectChanged)
    Q_PROPERTY(bool kittyGraphics READ kittyGraphics WRITE setKittyGraphics NOTIFY kittyGraphicsChanged)
    Q_PROPERTY(int clipboardReadPolicy READ clipboardReadPolicy WRITE setClipboardReadPolicy NOTIFY clipboardReadPolicyChanged)
    Q_PROPERTY(QString customShaderPath READ customShaderPath WRITE setCustomShaderPath NOTIFY customShaderPathChanged)
    Q_PROPERTY(bool shaderPipelineAvailable READ shaderPipelineAvailable NOTIFY shaderPipelineAvailableChanged)

public:
    // Session sort modes — values must match SessionPage.qml
    enum SessionSort { SortManual = 0, SortLastUsed = 1, SortCreated = 2, SortAlphabetical = 3 };
    Q_ENUM(SessionSort)

public:
    static Settings *instance();

    // Test constructor: allows injecting a custom settings path
    explicit Settings(const QString &settingsPath, QObject *parent = nullptr);

    // Exposed for SessionManager's group-based persistence
    QSettings &raw() { return m_settings; }

    // Public: called by SessionManager after writing via raw()
    void save();
    void scheduleSave();

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

    QStringList keybarKeys() const { return m_keybarKeys; }
    void setKeybarKeys(const QStringList &keys);

    bool keybarVisible() const { return m_keybarVisible; }
    void setKeybarVisible(bool visible);

    int sessionSortMode() const { return m_sessionSortMode; }
    void setSessionSortMode(int mode);

    bool cursorTrails() const { return m_cursorTrails; }
    void setCursorTrails(bool enabled);

    bool pinchToZoom() const { return m_pinchToZoom; }
    void setPinchToZoom(bool enabled);

    bool urlAutoDetect() const { return m_urlAutoDetect; }
    void setUrlAutoDetect(bool enabled);

    bool kittyGraphics() const { return m_kittyGraphics; }
    void setKittyGraphics(bool enabled);

    int clipboardReadPolicy() const { return m_clipboardReadPolicy; }
    void setClipboardReadPolicy(int policy);

    QString customShaderPath() const { return m_customShaderPath; }
    void setCustomShaderPath(const QString &path);

    bool shaderPipelineAvailable() const { return m_shaderPipelineAvailable; }
    void setShaderPipelineAvailable(bool available);


Q_SIGNALS:
    void fontSizeChanged();
    void fontFamilyChanged();
    void shellCommandChanged();
    void colorSchemeChanged();
    void backgroundOpacityChanged();
    void bellModeChanged();
    void scrollbackPersistenceChanged();
    void scrollbackRetentionDaysChanged();
    void keybarKeysChanged();
    void keybarVisibleChanged();
    void sessionSortModeChanged();
    void cursorTrailsChanged();
    void pinchToZoomChanged();
    void urlAutoDetectChanged();
    void kittyGraphicsChanged();
    void clipboardReadPolicyChanged();
    void customShaderPathChanged();
    void shaderPipelineAvailableChanged();

private:
    explicit Settings(QObject *parent = nullptr);
    void load();

    QSettings m_settings;
    QTimer *m_saveTimer;
    int m_fontSize = 18;
    QString m_fontFamily = QStringLiteral("monospace");
    QString m_shellCommand;
    QString m_colorScheme = QStringLiteral("dark");
    float m_backgroundOpacity = 0.6f;
    int m_bellMode = 1; // default: Vibrate
    bool m_scrollbackPersistence = false; // default: off (opt-in)
    int m_scrollbackRetentionDays = 30;
    QStringList m_keybarKeys;
    bool m_keybarVisible = true;
    int m_sessionSortMode = SortLastUsed; // default: sort by last used
    bool m_cursorTrails = true; // default: ON — matches load() default
    bool m_pinchToZoom = false; // default: OFF — pinch gesture changes font size
    bool m_urlAutoDetect = true; // default: ON — regex URL detection enabled
    bool m_kittyGraphics = true; // default: ON — Kitty Graphics Protocol
    int m_clipboardReadPolicy = 0; // 0=ask, 1=allow, 2=deny
    QString m_customShaderPath;
    bool m_shaderPipelineAvailable = false; // set by GLRenderer after ES 3.0 probe
};

#endif // SETTINGS_H
