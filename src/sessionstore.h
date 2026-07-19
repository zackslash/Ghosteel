#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class Settings;
class ScrollEncryptor;
class TerminalView;

// Session scrollback lifecycle (restore → dirty → save):
//   1. restoreSessions() creates each view, sets justRestored=true.
//      Geometry-update repaints fire contentChanged immediately, but
//      the handler no-ops while justRestored is true (avoids re-encrypting
//      just-restored scrollback on launch).
//   2. First real PTY byte arrives → titleChanged fires synchronously
//      (inside vtWrite, before update() emits contentChanged) → clears
//      justRestored. Subsequent contentChanged marks scrollbackDirty and
//      schedules a debounced save.
//   3. Debounce timer (500ms) or aboutToQuit → saveScrollbackIncremental()
//      encrypts only dirty sessions, active session first.
//   4. If encryption was unavailable at restore time, the file is queued
//      in the caller's pending-restore vector and retried once when
//      ScrollEncryptor::availabilityChanged fires.

// Session taxonomy (two orthogonal dimensions):
//
//                    No command (execArgs empty)   Command (execArgs set)
//  No name           Regular shell session         Anonymous command session
//  Named             Named shell session           Named command session
//
// Auto-remove: exit 0 → anonymous only; exit ≠ 0 → all command sessions.
// restartShell() clears execArgs → cancels pending auto-remove.
struct SessionInfo {
    int id;
    QString name;
    QString cachedWorkingDirectory; // Persisted CWD for inactive sessions
    QString autorunCommand;  // Command to run when session starts
    bool keybarOpen = true;           // Whether the extra keys panel is open
    bool keyboardVisible = true;      // Whether the software keyboard is visible
    bool keepAwake = false;           // Keep CPU awake when app is backgrounded
    int fontSize = 0;                 // Per-session font size (0 = use global default)
    QString execCommand;              // Command binary name from -e (display only)
    QStringList execArgs;             // Full command args including binary (for reuse matching)
    qint64 createdAt = 0;             // Epoch ms when session was created
    qint64 lastUsedAt = 0;            // Epoch ms when session was last switched to
    TerminalView *view;

    bool isAnonymous() const { return !execArgs.isEmpty() && name.isEmpty(); }
    bool isCommandSession() const { return !execArgs.isEmpty(); }
    bool scrollbackDirty = false;  // True if scrollback changed since last encrypt+save
    bool justRestored = false;     // True after restoreSessions(); skip dirty-marking until PTY data arrives
    qint64 lastScrollbackSaveMs = 0; // Epoch ms of last successful scrollback save (throttle under continuous output)
};

struct PendingScrollbackRestore {
    QPointer<TerminalView> view;
    int sessionId;
};

class SessionStore {
public:
    SessionStore(Settings *settings, ScrollEncryptor *encryptor);

    // Updates info.cachedWorkingDirectory and info.fontSize in-place from live view state.
    void saveSessionsMetadata(QVector<SessionInfo> &sessions, int activeIndex, int nextSessionId);

    // Encrypt + save scrollback for sessions marked dirty. Returns true if
    // throttled (5s min interval per session) and the caller should re-arm
    // the save timer; false otherwise. force=true bypasses the throttle
    // (used on aboutToQuit).
    bool saveScrollbackIncremental(QVector<SessionInfo> &sessions, int activeIndex, bool force = false);

    // purgeAll=true bypasses retention and removes every scrollback file (used when persistence is disabled).
    void cleanupScrollbackFiles(bool purgeAll = false);

    // If the file is encrypted but the encryptor isn't ready yet, append to pending for retry on availabilityChanged.
    void restoreScrollbackForSession(TerminalView *view, int savedId,
                                     QVector<PendingScrollbackRestore> &pending);

    QString scrollbackFilePath(int sessionId) const;

private:
    Settings *m_settings;
    ScrollEncryptor *m_encryptor;

    void saveSessionScrollback(SessionInfo &info);
    QString scrollbackDir() const;
};

#endif // SESSIONSTORE_H
