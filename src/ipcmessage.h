#ifndef IPCMESSAGE_H
#define IPCMESSAGE_H

#include <QByteArray>
#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>

// IPC protocol for single-instance communication.
struct IpcMessage {
    enum Type { Raise, Switch, Exec } type = Raise;
    QString sessionName;
    QString command;
    QStringList args;

    static constexpr int kMaxSessionNameLength = 128;

    static QString sanitizeSessionName(const QString &name) {
        QString clean = name;
        clean.truncate(kMaxSessionNameLength);
        clean.remove(QChar('\0'));
        clean.remove(QChar('\n'));
        clean.remove(QChar('\r'));
        clean.remove(QChar(':')); // load-bearing: exec: protocol uses : as delimiter
        return clean;
    }

    static IpcMessage parse(const QByteArray &raw) {
        IpcMessage msg;
        QList<QByteArray> parts = raw.split('\0');
        QByteArray header = parts.isEmpty() ? QByteArray() : parts.first();

        if (header == "raise") {
            msg.type = Raise;
        } else if (header.startsWith("switch:")) {
            msg.type = Switch;
            msg.sessionName = sanitizeSessionName(QString::fromUtf8(header.mid(7)));
        } else if (header.startsWith("exec:")) {
            msg.type = Exec;
            QByteArray afterPrefix = header.mid(5);
            int colonPos = afterPrefix.indexOf(':');
            if (colonPos < 0) { msg.type = Raise; return msg; }
            msg.sessionName = sanitizeSessionName(QString::fromUtf8(afterPrefix.left(colonPos)));
            if (colonPos + 1 < afterPrefix.size())
                msg.command = QString::fromUtf8(afterPrefix.mid(colonPos + 1));
            // Append every part unconditionally: an empty arg is a legitimate
            // value (encode() emits it as a bare '\0' separator, e.g.
            // `ghosteel -e grep '' foo`), so dropping it here would corrupt
            // the argv the primary receives. A trailing empty part only arises
            // when the last arg is genuinely empty (e.g. `ghosteel -e foo ''`
            // -> `exec::foo\0\n`), which the unconditional append preserves;
            // the chopped '\n' never creates a spurious one.
            for (int i = 1; i < parts.size(); i++)
                msg.args.append(QString::fromUtf8(parts[i]));
        }
        return msg;
    }

    static QByteArray encode(const QString &execCommand, const QStringList &execArgs, const QString &sessionName) {
        const QString cleanName = sanitizeSessionName(sessionName);
        if (!execCommand.isEmpty()) {
            QByteArray cmdBytes = execCommand.toUtf8();
            for (const QString &arg : execArgs) {
                cmdBytes.append('\0');
                cmdBytes.append(arg.toUtf8());
            }
            return (QStringLiteral("exec:") + cleanName + QStringLiteral(":")).toUtf8() + cmdBytes + '\n';
        } else if (!cleanName.isEmpty()) {
            return (QStringLiteral("switch:") + cleanName + QStringLiteral("\n")).toUtf8();
        }
        return QByteArrayLiteral("raise\n");
    }
};

#endif // IPCMESSAGE_H
