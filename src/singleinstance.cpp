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

void SessionManager::startSingleInstanceServer()
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
                qWarning() << "Ghosteel: Another instance detected via socket probe";
                failServer();
                return;
            }
            QLocalServer::removeServer(path);
            if (!m_localServer->listen(path)) {
                qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
                failServer();
                return;
            }
        } else {
            qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
            failServer();
            return;
        }
    }
    connect(m_localServer, &QLocalServer::newConnection,
            this, &SessionManager::onNewInstanceConnection);
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

    // Race: sender may have written and disconnected before we get here.
    auto processMessage = [this, socket]() {
        QByteArray data = socket->readAll().trimmed();
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
        connect(socket, &QLocalSocket::readyRead, this, processMessage);
        connect(socket, &QLocalSocket::disconnected, this, processMessage);
    }
}
