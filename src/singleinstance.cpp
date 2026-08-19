#include "sessionmanager.h"
#include "ipcmessage.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include <QGuiApplication>

QString SessionManager::socketPath()
{
    // Use XDG_RUNTIME_DIR directly — safe to call before QGuiApplication exists
    const QString runtime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (!runtime.isEmpty())
        return runtime + QStringLiteral("/ghosteel-singleton");
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
           + QStringLiteral("/ghosteel-singleton");
}

bool SessionManager::checkSingleInstance(const QString &execCommand,
                                         const QStringList &execArgs,
                                         const QString &sessionName)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (socket.waitForConnected(500)) {
        QByteArray msg = IpcMessage::encode(execCommand, execArgs, sessionName);
        socket.write(msg);
        socket.waitForBytesWritten(1000);
        socket.disconnectFromServer();
        return true;
    }
    return false;
}

bool SessionManager::startSingleInstanceServer()
{
    m_localServer = new QLocalServer(this);

    auto failServer = [this]() {
        delete m_localServer;
        m_localServer = nullptr;
    };

    const QString path = socketPath();

    if (!m_localServer->listen(path)) {
        if (m_localServer->serverError() == QAbstractSocket::AddressInUseError) {
            QLocalSocket probe;
            probe.connectToServer(path);
            if (probe.waitForConnected(200)) {
                // Another instance beat us to the socket during the startup
                // race (its server started listening after our initial
                // checkSingleInstance probe). Reuse this already-connected
                // socket to forward our pending CLI request to the primary,
                // best-effort, then exit.
                if (m_cliExecCommand.isEmpty() && m_cliSessionName.isEmpty()) {
                    // Plain duplicate launch — mirror checkSingleInstance's
                    // encoding (empty args encode as IpcMessage::Raise).
                    probe.write(IpcMessage::encode(QString(), QStringList(), QString()));
                } else {
                    // Forward the CLI request (Exec path — the primary will
                    // run it / switch to the requested session).
                    probe.write(IpcMessage::encode(m_cliExecCommand, m_cliExecArgs, m_cliSessionName));
                }
                probe.waitForBytesWritten(1000);
                probe.disconnectFromServer();
                qWarning() << "Ghosteel: Another instance detected via socket probe; duplicate exiting";
                failServer();
                return false;
            }
            QLocalServer::removeServer(path);
            if (!m_localServer->listen(path)) {
                qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
                failServer();
                return true;
            }
        } else {
            qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
            failServer();
            return true;
        }
    }
    connect(m_localServer, &QLocalServer::newConnection,
            this, &SessionManager::onNewInstanceConnection);
    return true;
}

void SessionManager::setCliArgs(const QString &execCommand,
                                const QStringList &execArgs,
                                const QString &sessionName)
{
    m_cliExecCommand = execCommand;
    m_cliExecArgs = execArgs;
    m_cliSessionName = IpcMessage::sanitizeSessionName(sessionName);
}

void SessionManager::clearCliArgs()
{
    m_cliExecCommand.clear();
    m_cliExecArgs.clear();
    m_cliSessionName.clear();
}

void SessionManager::raiseWindow()
{
    const auto windows = QGuiApplication::topLevelWindows();
    if (!windows.isEmpty()) {
        if (auto *window = windows.first()) {
            window->raise();
            window->requestActivate();
        }
    }
}

void SessionManager::onNewInstanceConnection()
{
    QLocalSocket *socket = m_localServer->nextPendingConnection();
    if (!socket) return;
    // nextPendingConnection() returns an unparented socket — a peer that
    // connects then neither writes nor disconnects would otherwise leak fd +
    // object until exit. Parenting ties teardown to the manager (the
    // deleteLater paths below still work on a parented object).
    socket->setParent(this);

    // Race: sender may have written and disconnected before we get here.
    auto processMessage = [this, socket]() {
        // Bounded read: 64 KiB caps memory use in this intentionally
        // unauthenticated IPC design. Read one byte past the cap so an
        // oversized message is detected and discarded whole — truncating it
        // would cut mid-args and still run a mangled command. (Only the
        // session name is bounded at 128 chars; command/args are unbounded
        // user input.)
        QByteArray data = socket->read(64 * 1024 + 1);
        if (data.size() > 64 * 1024) {
            qWarning() << "Ghosteel: IPC message exceeds 64 KiB, discarding";
            socket->deleteLater();
            return;
        }
        // Chop exactly one trailing newline (encode() appends '\n'); unlike
        // trimmed(), this preserves trailing whitespace in a forwarded last
        // arg such as `sh -c 'echo hi '`.
        if (data.endsWith('\n'))
            data.chop(1);
        socket->deleteLater();

        IpcMessage parsed = IpcMessage::parse(data);
        if (parsed.type == IpcMessage::Raise) {
            raiseWindow();
        } else if (parsed.type == IpcMessage::Switch) {
            switchToSessionByName(parsed.sessionName);
            raiseWindow();
        } else if (parsed.type == IpcMessage::Exec) {
            if (parsed.command.isEmpty()) return;
            setCliArgs(parsed.command, parsed.args, parsed.sessionName);
            processCliArgs();
            // Delay raise to let QML process sessionCreated() signal
            QTimer::singleShot(100, this, raiseWindow);
        }
    };

    if (socket->bytesAvailable() > 0 || socket->state() == QLocalSocket::UnconnectedState) {
        processMessage();
    } else {
        // disconnected also covers: client connects, never writes, drops.
        // Use a shared guard so readyRead + disconnected can't double-fire.
        auto guard = std::make_shared<bool>(false);
        connect(socket, &QLocalSocket::readyRead, this, [processMessage, guard]() {
            if (*guard) return;
            *guard = true;
            processMessage();
        });
        connect(socket, &QLocalSocket::disconnected, this, [processMessage, guard]() {
            if (*guard) return;
            *guard = true;
            processMessage();
        });
    }
}
