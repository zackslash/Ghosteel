#include <QtTest>

#include "ipcmessage.h"

class TestIpcMessage : public QObject
{
    Q_OBJECT

private slots:
    // parse() receives the wire bytes after the receiver chops the trailing
    // '\n' (encode() appends it); mirror that here.
    static IpcMessage parseEncoded(const QByteArray &raw)
    {
        QByteArray data = raw;
        if (data.endsWith('\n'))
            data.chop(1);
        return IpcMessage::parse(data);
    }

    void raiseRoundTrip()
    {
        QByteArray raw = IpcMessage::encode(QString(), QStringList(), QString());
        IpcMessage msg = parseEncoded(raw);
        QCOMPARE(msg.type, IpcMessage::Raise);
        QCOMPARE(msg.restart, false);
        QVERIFY(msg.sessionName.isEmpty());
        QVERIFY(msg.command.isEmpty());
        QVERIFY(msg.args.isEmpty());
    }

    void switchRoundTrip()
    {
        // Colon is stripped by sanitizeSessionName (protocol delimiter)
        QByteArray raw = IpcMessage::encode(QString(), QStringList(), "we:ird\nname");
        IpcMessage msg = parseEncoded(raw);
        QCOMPARE(msg.type, IpcMessage::Switch);
        QCOMPARE(msg.restart, false);
        QCOMPARE(msg.sessionName, QStringLiteral("weirdname"));
    }

    void execRoundTrip()
    {
        QStringList args{QStringLiteral("-s"), QStringLiteral("PERCENT_CPU"), QString()};
        QByteArray raw = IpcMessage::encode(QStringLiteral("htop"), args, QStringLiteral("sysmon"));
        IpcMessage msg = parseEncoded(raw);
        QCOMPARE(msg.type, IpcMessage::Exec);
        QCOMPARE(msg.restart, false);
        QCOMPARE(msg.sessionName, QStringLiteral("sysmon"));
        QCOMPARE(msg.command, QStringLiteral("htop"));
        QCOMPARE(msg.args.size(), 3);
        QCOMPARE(msg.args.at(0), QStringLiteral("-s"));
        QCOMPARE(msg.args.at(1), QStringLiteral("PERCENT_CPU"));
        // Empty-string arg must survive the round trip
        QCOMPARE(msg.args.at(2), QString());
    }

    void execRestart()
    {
        QByteArray raw = IpcMessage::encode(QStringLiteral("top"), QStringList(), QStringLiteral("sysmon"), true);
        QVERIFY(raw.startsWith("exec!:"));
        IpcMessage msg = parseEncoded(raw);
        QCOMPARE(msg.type, IpcMessage::Exec);
        QCOMPARE(msg.restart, true);
        QCOMPARE(msg.sessionName, QStringLiteral("sysmon"));
        QCOMPARE(msg.command, QStringLiteral("top"));
        QVERIFY(msg.args.isEmpty());
    }

    void execWithoutRestart()
    {
        QByteArray raw = IpcMessage::encode(QStringLiteral("top"), QStringList(), QStringLiteral("sysmon"));
        QVERIFY(raw.startsWith("exec:"));
        IpcMessage msg = parseEncoded(raw);
        QCOMPARE(msg.type, IpcMessage::Exec);
        QCOMPARE(msg.restart, false);
        QCOMPARE(msg.command, QStringLiteral("top"));
    }

    void malformedExecDegradesToRaise()
    {
        // No second colon after the prefix
        IpcMessage msg = parseEncoded(QByteArray("exec:nocolon\n"));
        QCOMPARE(msg.type, IpcMessage::Raise);
        QCOMPARE(msg.restart, false);

        IpcMessage msgRestart = parseEncoded(QByteArray("exec!:nocolon\n"));
        QCOMPARE(msgRestart.type, IpcMessage::Raise);
        QCOMPARE(msgRestart.restart, false);
    }

    void anonymousRestartRoundTrip()
    {
        QByteArray raw = IpcMessage::encode(QStringLiteral("grep"),
                                            {QStringLiteral("foo"), QString()},
                                            QString(), true);
        QVERIFY(raw.startsWith("exec!::grep"));
        IpcMessage msg = parseEncoded(raw);
        QCOMPARE(msg.type, IpcMessage::Exec);
        QCOMPARE(msg.restart, true);
        QVERIFY(msg.sessionName.isEmpty());
        QCOMPARE(msg.command, QStringLiteral("grep"));
        QCOMPARE(msg.args.size(), 2);
        QCOMPARE(msg.args.at(0), QStringLiteral("foo"));
        QCOMPARE(msg.args.at(1), QString());
    }

    void switchIgnoresRestart()
    {
        QByteArray raw = IpcMessage::encode(QString(), QStringList(), QStringLiteral("sysmon"), true);
        QVERIFY(raw.startsWith("switch:"));
        IpcMessage msg = parseEncoded(raw);
        QCOMPARE(msg.type, IpcMessage::Switch);
        QCOMPARE(msg.restart, false);
        QCOMPARE(msg.sessionName, QStringLiteral("sysmon"));
    }
};

QTEST_MAIN(TestIpcMessage)
#include "tst_ipcmessage.moc"
