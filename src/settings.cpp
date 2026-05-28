#include "settings.h"

#include <QStandardPaths>

Settings::Settings(QObject *parent)
    : QObject(parent)
    , m_settings(
          QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
          + QStringLiteral("/" APP_ORG "/" APP_NAME ".conf"),
          QSettings::IniFormat)
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
                                    QStringLiteral("DejaVu Sans Mono")).toString();
    m_shellCommand = m_settings.value(QStringLiteral("terminal/shell"), QString()).toString();
    m_colorScheme = m_settings.value(QStringLiteral("terminal/colorScheme"),
                                     QStringLiteral("dark")).toString();
    m_backgroundOpacity = m_settings.value(QStringLiteral("terminal/backgroundOpacity"), 0.6f).toFloat();
    m_bellMode = qBound(0, m_settings.value(QStringLiteral("terminal/bellMode"), 1).toInt(), 3);
}

void Settings::save()
{
    m_settings.sync();
}

void Settings::scheduleSave()
{
    m_saveTimer->start(); // restarts timer on each call (debounce)
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