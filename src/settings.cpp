#include "settings.h"

#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QDebug>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

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
    // Clean up stale temp file from a previous crash during atomic save
    QFile::remove(settingsPath + QStringLiteral(".tmp"));

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
    // Non-numeric values (corrupt/legacy entries) must fall back to the
    // default before clamping — toInt() on garbage yields 0, which would
    // clamp to the range sentinel instead.
    bool ok = false;
    int v = m_settings.value(QStringLiteral("font/size"), 18).toInt(&ok);
    if (!ok) v = 18;
    m_fontSize = qBound(kMinFontSize, v, kMaxFontSize);
    m_fontFamily = m_settings.value(QStringLiteral("font/family"),
                                    QStringLiteral("monospace")).toString();
    m_shellCommand = m_settings.value(QStringLiteral("terminal/shell"), QString()).toString();
    m_colorScheme = m_settings.value(QStringLiteral("terminal/colorScheme"),
                                     QStringLiteral("dark")).toString();
    m_followAmbience = m_settings.value(QStringLiteral("terminal/followAmbience"), true).toBool();
    ok = false;
    float f = m_settings.value(QStringLiteral("terminal/backgroundOpacity"), 0.6f).toFloat(&ok);
    if (!ok) f = 0.6f;
    m_backgroundOpacity = qBound(0.0f, f, 1.0f);
    ok = false;
    v = m_settings.value(QStringLiteral("terminal/bellMode"), 1).toInt(&ok);
    if (!ok) v = 1;
    m_bellMode = qBound(0, v, 3);
    m_scrollbackPersistence = m_settings.value(QStringLiteral("scrollback/enabled"), false).toBool();
    ok = false;
    v = m_settings.value(QStringLiteral("scrollback/retentionDays"), 30).toInt(&ok);
    if (!ok) v = 30;
    m_scrollbackRetentionDays = qBound(7, v, 365);
    QStringList defaultKeys = {QStringLiteral("left"), QStringLiteral("down"), QStringLiteral("up"),
                               QStringLiteral("right"), QStringLiteral("tab"), QStringLiteral("ctrl"),
                               QStringLiteral("alt"), QStringLiteral("esc"), QStringLiteral("keyboard")};
    m_keybarKeys = m_settings.value(QStringLiteral("keybar/keys"), QVariant::fromValue(defaultKeys)).toStringList();
    m_keybarVisible = m_settings.value(QStringLiteral("keybar/visible"), true).toBool();
    m_keybarRowBreaks = sanitizeRowBreaks(m_settings.value(QStringLiteral("keybar/rowBreaks")).toList());
    ok = false;
    v = m_settings.value(QStringLiteral("sessions/sortMode"), SortLastUsed).toInt(&ok);
    if (!ok) v = SortLastUsed;
    m_sessionSortMode = qBound(0, v, 3);
    m_cursorTrails = m_settings.value(QStringLiteral("terminal/cursorTrails"), true).toBool();
    m_pinchToZoom = m_settings.value(QStringLiteral("terminal/pinchToZoom"), false).toBool();
    m_autoHideKeyboardLandscape = m_settings.value(QStringLiteral("terminal/autoHideKeyboardLandscape"), false).toBool();
    m_urlAutoDetect = m_settings.value(QStringLiteral("terminal/urlAutoDetect"), true).toBool();
    m_kittyGraphics = m_settings.value(QStringLiteral("terminal/kittyGraphics"), true).toBool();
    ok = false;
    v = m_settings.value(QStringLiteral("terminal/clipboardReadPolicy"), 0).toInt(&ok);
    if (!ok) v = 0;
    m_clipboardReadPolicy = qBound(0, v, 2);
    m_customShaderPath = m_settings.value(QStringLiteral("terminal/customShaderPath")).toString();
}

void Settings::save()
{
    const QString path = m_settings.fileName();
    const QString tmpPath = path + QStringLiteral(".tmp");

    // QSettings preserves INI formatting, group structure, and type handling
    {
        QSettings tmp(tmpPath, QSettings::IniFormat);
        const auto keys = m_settings.allKeys();
        for (const auto &key : keys)
            tmp.setValue(key, m_settings.value(key));
        tmp.sync();
    }

    // fsync before rename to ensure data is on disk — protects against
    // power loss between write and rename (same pattern as scrollback)
    {
        QFile f(tmpPath);
        if (f.open(QIODevice::ReadOnly))
            ::fsync(f.handle());
    }

    if (QFile::exists(tmpPath)) {
        if (::rename(tmpPath.toUtf8().constData(), path.toUtf8().constData()) != 0) {
            qWarning() << "Settings: atomic save failed:" << std::strerror(errno);
            QFile::remove(tmpPath);
            m_settings.sync();
        } else {
            // fsync the parent directory so the rename's dirent update
            // survives a power loss (file content is already durable).
            int dirFd = ::open(QFileInfo(path).absolutePath().toUtf8().constData(), O_RDONLY);
            if (dirFd >= 0) {
                if (::fsync(dirFd) != 0)
                    qWarning() << "Settings: dir fsync failed:" << std::strerror(errno);
                ::close(dirFd);
            } else {
                qWarning() << "Settings: dir fsync open failed:" << std::strerror(errno);
            }
        }
    } else {
        m_settings.sync();
    }
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

void Settings::setFollowAmbience(bool enabled)
{
    if (m_followAmbience == enabled)
        return;
    m_followAmbience = enabled;
    m_settings.setValue(QStringLiteral("terminal/followAmbience"), enabled);
    scheduleSave();
    Q_EMIT followAmbienceChanged();
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

QVariantList Settings::sanitizeRowBreaks(const QVariantList &breaks) const
{
    // Clamp to at most 2 entries (max 3 rows), filter invalid values,
    // drop breaks past the end of keybarKeys (prevents empty trailing rows),
    // drop out-of-order and duplicate entries (monotonic guard enforces sorting).
    QVariantList clamped;
    for (int i = 0; i < breaks.size() && clamped.size() < 2; ++i) {
        bool ok = false;
        int val = breaks[i].toInt(&ok);
        if (ok && val > 0 && val < m_keybarKeys.size()
            && (clamped.isEmpty() || val > clamped.last().toInt()))
            clamped.append(val);
    }
    return clamped;
}

void Settings::setKeybarRowBreaks(const QVariantList &breaks)
{
    QVariantList clamped = sanitizeRowBreaks(breaks);
    if (m_keybarRowBreaks == clamped)
        return;
    m_keybarRowBreaks = clamped;
    m_settings.setValue(QStringLiteral("keybar/rowBreaks"), clamped);
    scheduleSave();
    Q_EMIT keybarRowBreaksChanged();
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

void Settings::setClipboardReadPolicy(int policy)
{
    if (policy < 0) policy = 0;
    if (policy > 2) policy = 2;
    if (m_clipboardReadPolicy == policy)
        return;
    m_clipboardReadPolicy = policy;
    m_settings.setValue(QStringLiteral("terminal/clipboardReadPolicy"), policy);
    scheduleSave();
    Q_EMIT clipboardReadPolicyChanged();
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

void Settings::setPinchToZoom(bool enabled)
{
    if (m_pinchToZoom == enabled)
        return;
    m_pinchToZoom = enabled;
    m_settings.setValue(QStringLiteral("terminal/pinchToZoom"), enabled);
    scheduleSave();
    Q_EMIT pinchToZoomChanged();
}

void Settings::setAutoHideKeyboardLandscape(bool enabled)
{
    if (m_autoHideKeyboardLandscape == enabled)
        return;
    m_autoHideKeyboardLandscape = enabled;
    m_settings.setValue(QStringLiteral("terminal/autoHideKeyboardLandscape"), enabled);
    scheduleSave();
    Q_EMIT autoHideKeyboardLandscapeChanged();
}

void Settings::setShaderPipelineAvailable(bool available)
{
    if (m_shaderPipelineAvailable == available)
        return;
    m_shaderPipelineAvailable = available;
    Q_EMIT shaderPipelineAvailableChanged();
}

