#include "sessionstore.h"
#include "settings.h"
#include "scrollencryptor.h"
#include "terminalview.h"

#include <QStandardPaths>
#include <QDir>
#include <QSaveFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {
// Cap restored scrollback file size to prevent pathological memory use.
constexpr qint64 kMaxScrollbackFileBytes = 4 * 1024 * 1024;
}   // namespace

SessionStore::SessionStore(Settings *settings, ScrollEncryptor *encryptor)
    : QObject(nullptr)
    , m_settings(settings)
    , m_encryptor(encryptor)
{
}

void SessionStore::saveSessionsMetadata(QVector<SessionInfo> &sessions, int activeIndex, int nextSessionId)
{
    QSettings &s = m_settings->raw();

    // Clear old session entries
    s.remove(QStringLiteral("sessionData"));

    // Skip anonymous command sessions during save
    int saveIndex = 0;
    for (SessionInfo &info : sessions) {

        if (info.isAnonymous())
            continue;

        QString group = QStringLiteral("sessionData/session_%1").arg(saveIndex);
        s.beginGroup(group);
        s.setValue(QStringLiteral("id"), info.id);
        s.setValue(QStringLiteral("name"), info.name);
        // Use live CWD from /proc if shell is running, otherwise use cached value
        if (info.view) {
            QString liveCwd = info.view->workingDirectory();
            if (!liveCwd.isEmpty())
                info.cachedWorkingDirectory = liveCwd;
        }
        QString cwd = info.cachedWorkingDirectory;
        if (cwd.isEmpty())
            cwd = QDir::homePath();
        s.setValue(QStringLiteral("workingDirectory"), cwd);
        s.setValue(QStringLiteral("autorunCommand"), info.autorunCommand);
        // Persist per-session font size. When fontSize == 0 (track global
        // default), don't read the live view size — that would clobber the
        // sentinel with the resolved global value.
        if (info.view && info.fontSize > 0)
            info.fontSize = info.view->fontSize();
        s.setValue(QStringLiteral("fontSize"), info.fontSize);
        s.setValue(QStringLiteral("keybarOpen"), info.keybarOpen);
        s.setValue(QStringLiteral("keyboardVisible"), info.keyboardVisible);
        s.setValue(QStringLiteral("createdAt"), info.createdAt);
        s.setValue(QStringLiteral("lastUsedAt"), info.lastUsedAt);
        s.endGroup();
        saveIndex++;
    }

    s.beginGroup(QStringLiteral("sessions"));
    s.setValue(QStringLiteral("count"), saveIndex);
    s.setValue(QStringLiteral("nextId"), nextSessionId);
    
    int activeSessionId = (activeIndex >= 0
                           && activeIndex < sessions.size())
                          ? sessions[activeIndex].id : -1;
    s.setValue(QStringLiteral("activeId"), activeSessionId);
    s.endGroup();

    m_settings->save();
}

QString SessionStore::scrollbackDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/scrollback");
}

QString SessionStore::scrollbackFilePath(int sessionId) const
{
    return scrollbackDir() + QStringLiteral("/session_%1.vt").arg(sessionId);
}

void SessionStore::saveSessionScrollback(SessionInfo &info, bool forceSync)
{
    if (!info.view)
        return;

    uint16_t cols = 0, rows = 0;
    QByteArray data = info.view->exportScrollback(cols, rows);
    if (data.isEmpty()) {
        // Nothing to persist. Route through saveCompleted so the manager's
        // handler updates scrollbackDirty and the save-throttle timestamp the
        // same way a real (encrypted) save does — otherwise the throttle
        // never engages after an empty export.
        Q_EMIT saveCompleted(info.id);
        return;
    }

    if (!m_encryptor)
        return; // No encryption subsystem — leave dirty for later.

    if (forceSync) {
        // aboutToQuit / destructor fallback: the event loop is stopping, so an
        // async D-Bus reply would never be delivered. Blocking here is fine —
        // the GUI is going away and persistence must be guaranteed.
        const QByteArray output = m_encryptor->encrypt(data);
        if (output.isEmpty()) {
            qWarning() << "Ghosteel: Scrollback encryption failed for session"
                       << info.id << ", skipping";
            return; // Don't write plaintext to disk; leave dirty.
        }
        writeScrollbackToDisk(info.id, output);
        return;
    }

    // Async path: startRequest() returns immediately and the callback fires on
    // the GUI thread event loop when the crypto daemon replies. No
    // waitForFinished(), so the GUI thread never stalls on D-Bus.
    const int sessionId = info.id;
    info.scrollbackSaveInFlight = true;
    const bool started = m_encryptor->encryptAsync(data,
        [this, sessionId](const QByteArray &encrypted) {
            writeScrollbackToDisk(sessionId, encrypted);
        });
    if (!started) {
        // Encryption unavailable — leave dirty for a later retry.
        info.scrollbackSaveInFlight = false;
        qWarning() << "Ghosteel: Async scrollback encryption unavailable for session"
                   << sessionId << ", leaving dirty for retry";
    }
}

void SessionStore::writeScrollbackToDisk(int sessionId, const QByteArray &encrypted)
{
    // Persistence may have been disabled while the D-Bus request was in flight
    // — don't recreate a file the purge just removed.
    if (!m_settings->scrollbackPersistence()) {
        Q_EMIT saveFailed(sessionId);
        return;
    }

    if (encrypted.isEmpty()) {
        qWarning() << "Ghosteel: Scrollback encryption failed for session"
                   << sessionId << ", skipping";
        Q_EMIT saveFailed(sessionId);
        return; // Don't write plaintext to disk
    }

    // Atomic write via QSaveFile — commit() is a POSIX rename(),
    // which is atomic on the same filesystem. No window where both
    // old and new files are gone.
    QSaveFile saveFile(scrollbackFilePath(sessionId));
    if (saveFile.open(QIODevice::WriteOnly)) {
        if (saveFile.write(encrypted) != -1) {
            // fsync before rename to ensure data is on disk —
            // protects against power loss between write and rename
            ::fsync(saveFile.handle());
            if (saveFile.commit()) {
                // fsync the parent directory so the rename's dirent update
                // survives a power loss (file content is already durable).
                int dirFd = ::open(scrollbackDir().toUtf8().constData(), O_RDONLY);
                if (dirFd >= 0) {
                    if (::fsync(dirFd) != 0)
                        qWarning() << "Scrollback: dir fsync failed:" << std::strerror(errno);
                    ::close(dirFd);
                } else {
                    qWarning() << "Scrollback: dir fsync open failed:" << std::strerror(errno);
                }
                Q_EMIT saveCompleted(sessionId);
                return;
            }
            qWarning() << "Failed to commit scrollback:" << saveFile.errorString();
        } else {
            qWarning() << "Failed to write scrollback:" << saveFile.errorString();
        }
    } else {
        qWarning() << "Failed to open scrollback:" << saveFile.errorString();
    }
    Q_EMIT saveFailed(sessionId);
}

bool SessionStore::saveScrollbackIncremental(QVector<SessionInfo> &sessions, int activeIndex, bool force)
{
    if (!m_settings->scrollbackPersistence())
        return false;

    QDir().mkpath(scrollbackDir());

    // Throttle scrollback saves under continuous output (tail -f, builds):
    // exportScrollback + Sailfish-Secrets D-Bus + fsync is expensive on a
    // handset, and contentChanged fires every repaint, so an unthrottled
    // 500ms debounce would re-encrypt ~2x/sec. Cap each session to one save
    // per kMinScrollbackIntervalMs; force=true (quit) bypasses it. A sporadic
    // edit landing inside a window is deferred up to ~5s — acceptable for
    // scrollback durability (quit always force-saves).
    static constexpr qint64 kMinScrollbackIntervalMs = 5000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool anyThrottled = false;

    auto trySave = [&](SessionInfo &info) {
        // Skip sessions with an async encrypt request still in flight —
        // avoid a second concurrent encrypt racing the in-flight one.
        if (!force && info.scrollbackSaveInFlight)
            return;
        if (!info.scrollbackDirty)
            return;  // cheap pre-check; saveSessionScrollback does not re-check dirty
        if (!force && info.lastScrollbackSaveMs > 0
            && (now - info.lastScrollbackSaveMs) < kMinScrollbackIntervalMs) {
            // Stay dirty; caller re-arms via the throttled return value.
            anyThrottled = true;
            return;
        }
        saveSessionScrollback(info, force);  // async (or sync on quit); dirty cleared by saveCompleted handler
    };

    // Process active session first for priority on quit
    if (activeIndex >= 0 && activeIndex < sessions.size())
        trySave(sessions[activeIndex]);

    for (int i = 0; i < sessions.size(); i++) {
        if (i == activeIndex)
            continue;
        trySave(sessions[i]);
    }

    // Returns anyThrottled so the caller re-arms. Throttled sessions skip
    // exportScrollback entirely, so the intervening 500ms fires stay cheap.
    return anyThrottled;
}

void SessionStore::cleanupScrollbackFiles(bool purgeAll)
{
    int retentionDays = m_settings->scrollbackRetentionDays();
    QDir dir(scrollbackDir());
    if (!dir.exists())
        return;

    QDateTime cutoff = QDateTime::currentDateTime().addDays(-retentionDays);
    const QFileInfoList files = dir.entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        if (fi.fileName().startsWith(QStringLiteral("session_"))) {
            if (fi.fileName().endsWith(QStringLiteral(".vt"))) {
                if (purgeAll || fi.lastModified() < cutoff)
                    QFile::remove(fi.absoluteFilePath());
            } else {
                // Orphaned QSaveFile temp file from a crash during write.
                // These have random suffixes (e.g. session_1.vt.aBcDeF).
                // Safe to remove — they're never read on restore.
                QFile::remove(fi.absoluteFilePath());
            }
        }
    }
}

void SessionStore::restoreScrollbackForSession(TerminalView *view, int savedId,
                                               QVector<PendingScrollbackRestore> &pending)
{
    if (!m_settings->scrollbackPersistence())
        return;

    QString sbPath = scrollbackFilePath(savedId);
    QFile sbFile(sbPath);
    if (!sbFile.exists() || !sbFile.open(QIODevice::ReadOnly))
        return;

    if (sbFile.size() > kMaxScrollbackFileBytes) {
        qWarning() << "Scrollback file too large, skipping:" << sbPath;
        return;
    }

    QByteArray sbData = sbFile.readAll();
    if (sbData.isEmpty())
        return;

    const bool encrypted = ScrollEncryptor::isEncryptedFormat(sbData);
    if (encrypted && m_encryptor && m_encryptor->isAvailable()) {
        QByteArray restored = m_encryptor->decrypt(sbData);
        if (!restored.isEmpty())
            view->setPendingScrollback(restored);
    } else if (encrypted) {
        // Encryption not yet available — queue for retry after availabilityChanged
        pending.append({view, savedId});
    }
}
