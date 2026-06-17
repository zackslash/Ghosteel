#include "settings.h"

#include <QStandardPaths>

Settings::Settings(QObject *parent)
    : Settings(
          QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
          + QStringLiteral("/" APP_ORG "/" APP_NAME ".conf"),
          parent)
{}

Settings::Settings(const QString &settingsPath, QObject *parent)
    : QObject(parent)
    , m_settings(settingsPath, QSettings::IniFormat)
{
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500); // 500ms debounce
    connect(m_saveTimer, &QTimer::timeout, this, &Settings::save);
    load();
}

Settings *Settings::instance()
{
    static Settings s;
    return &s;
}

void Settings::load()
{
    m_fontSize = m_settings.value(QStringLiteral("font/size"), 18).toInt();
    m_fontFamily = m_settings.value(QStringLiteral("font/family"),
                                    QStringLiteral("monospace")).toString();
    m_shellCommand = m_settings.value(QStringLiteral("terminal/shell"), QString()).toString();
    m_colorScheme = m_settings.value(QStringLiteral("terminal/colorScheme"),
                                     QStringLiteral("dark")).toString();
    m_backgroundOpacity = qBound(0.0f, m_settings.value(QStringLiteral("terminal/backgroundOpacity"), 0.6f).toFloat(), 1.0f);
    m_bellMode = qBound(0, m_settings.value(QStringLiteral("terminal/bellMode"), 1).toInt(), 3);
    m_scrollbackPersistence = m_settings.value(QStringLiteral("scrollback/enabled"), false).toBool();
    m_scrollbackRetentionDays = qBound(7, m_settings.value(QStringLiteral("scrollback/retentionDays"), 30).toInt(), 365);
    QStringList defaultKeys = {QStringLiteral("left"), QStringLiteral("down"), QStringLiteral("up"),
                               QStringLiteral("right"), QStringLiteral("tab"), QStringLiteral("ctrl"),
                               QStringLiteral("alt"), QStringLiteral("keyboard"), QStringLiteral("esc")};
    m_keybarKeys = m_settings.value(QStringLiteral("keybar/keys"), QVariant::fromValue(defaultKeys)).toStringList();
    m_keybarVisible = m_settings.value(QStringLiteral("keybar/visible"), true).toBool();
    m_sessionSortMode = qBound(0, m_settings.value(QStringLiteral("sessions/sortMode"), SortLastUsed).toInt(), 3);
    m_cursorTrails = m_settings.value(QStringLiteral("terminal/cursorTrails"), true).toBool();
    m_urlAutoDetect = m_settings.value(QStringLiteral("terminal/urlAutoDetect"), true).toBool();
    m_kittyGraphics = m_settings.value(QStringLiteral("terminal/kittyGraphics"), true).toBool();
    m_customShaderPath = m_settings.value(QStringLiteral("terminal/customShaderPath")).toString();
}

void Settings::save()
{
    m_settings.sync();
}

void Settings::scheduleSave()
{
    m_saveTimer->start();
}

void Settings::setFontSize(int size)
{
    if (m_fontSize == size)
        return;
    m_fontSize = size;
    m_settings.setValue(QStringLiteral("font/size"), size);
    scheduleSave();
    Q_EMIT fontSizeChanged();
}

void Settings::setFontFamily(const QString &family)
{
    if (m_fontFamily == family)
        return;
    m_fontFamily = family;
    m_settings.setValue(QStringLiteral("font/family"), family);
    scheduleSave();
    Q_EMIT fontFamilyChanged();
}

void Settings::setShellCommand(const QString &cmd)
{
    if (m_shellCommand == cmd)
        return;
    m_shellCommand = cmd;
    m_settings.setValue(QStringLiteral("terminal/shell"), cmd);
    scheduleSave();
    Q_EMIT shellCommandChanged();
}

void Settings::setColorScheme(const QString &scheme)
{
    if (m_colorScheme == scheme)
        return;
    m_colorScheme = scheme;
    m_settings.setValue(QStringLiteral("terminal/colorScheme"), scheme);
    scheduleSave();
    Q_EMIT colorSchemeChanged();
}

void Settings::setBackgroundOpacity(float opacity)
{
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    if (qAbs(m_backgroundOpacity - opacity) < 0.001f)
        return;
    m_backgroundOpacity = opacity;
    m_settings.setValue(QStringLiteral("terminal/backgroundOpacity"), opacity);
    scheduleSave();
    Q_EMIT backgroundOpacityChanged();
}

void Settings::setBellMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    if (m_bellMode == mode)
        return;
    m_bellMode = mode;
    m_settings.setValue(QStringLiteral("terminal/bellMode"), mode);
    scheduleSave();
    Q_EMIT bellModeChanged();
}

void Settings::setScrollbackPersistence(bool enabled)
{
    if (m_scrollbackPersistence == enabled)
        return;
    m_scrollbackPersistence = enabled;
    m_settings.setValue(QStringLiteral("scrollback/enabled"), enabled);
    scheduleSave();
    Q_EMIT scrollbackPersistenceChanged();
}

void Settings::setScrollbackRetentionDays(int days)
{
    if (days < 7) days = 7;
    if (days > 365) days = 365;
    if (m_scrollbackRetentionDays == days)
        return;
    m_scrollbackRetentionDays = days;
    m_settings.setValue(QStringLiteral("scrollback/retentionDays"), days);
    scheduleSave();
    Q_EMIT scrollbackRetentionDaysChanged();
}

void Settings::setKeybarKeys(const QStringList &keys)
{
    if (m_keybarKeys == keys)
        return;
    m_keybarKeys = keys;
    m_settings.setValue(QStringLiteral("keybar/keys"), QVariant::fromValue(keys));
    scheduleSave();
    Q_EMIT keybarKeysChanged();
}

void Settings::setKeybarVisible(bool visible)
{
    if (m_keybarVisible == visible)
        return;
    m_keybarVisible = visible;
    m_settings.setValue(QStringLiteral("keybar/visible"), visible);
    scheduleSave();
    Q_EMIT keybarVisibleChanged();
}

void Settings::setSessionSortMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    if (m_sessionSortMode == mode)
        return;
    m_sessionSortMode = mode;
    m_settings.setValue(QStringLiteral("sessions/sortMode"), mode);
    scheduleSave();
    Q_EMIT sessionSortModeChanged();
}

void Settings::setCursorTrails(bool enabled)
{
    if (m_cursorTrails == enabled)
        return;
    m_cursorTrails = enabled;
    m_settings.setValue(QStringLiteral("terminal/cursorTrails"), enabled);
    scheduleSave();
    Q_EMIT cursorTrailsChanged();
}

void Settings::setUrlAutoDetect(bool enabled)
{
    if (m_urlAutoDetect == enabled)
        return;
    m_urlAutoDetect = enabled;
    m_settings.setValue(QStringLiteral("terminal/urlAutoDetect"), enabled);
    scheduleSave();
    Q_EMIT urlAutoDetectChanged();
}

void Settings::setKittyGraphics(bool enabled)
{
    if (m_kittyGraphics == enabled)
        return;
    m_kittyGraphics = enabled;
    m_settings.setValue(QStringLiteral("terminal/kittyGraphics"), enabled);
    scheduleSave();
    Q_EMIT kittyGraphicsChanged();
}

void Settings::setCustomShaderPath(const QString &path)
{
    if (m_customShaderPath == path)
        return;
    m_customShaderPath = path;
    m_settings.setValue(QStringLiteral("terminal/customShaderPath"), path);
    scheduleSave();
    Q_EMIT customShaderPathChanged();
}

void Settings::setShaderPipelineAvailable(bool available)
{
    if (m_shaderPipelineAvailable == available)
        return;
    m_shaderPipelineAvailable = available;
    Q_EMIT shaderPipelineAvailableChanged();
}

