// Include terminalview.h BEFORE Qt headers to ensure Ghostty's C headers
// are parsed before Qt defines the `emit` macro (which conflicts with
// Ghostty's use of 'emit' as a struct field name).
#include "terminalview.h"
#include "glrenderer.h"
#include "sessionmanager.h"
#include "settings.h"

#ifdef QT_QML_DEBUG
#include <QtQuick>
#endif

#include <sailfishapp.h>
#include <QGuiApplication>
#include <QQuickView>
#include <QtQml>
#include <QQmlContext>
#include <QTranslator>
#include <QLocale>
#include <QDBusConnection>
#include <QDBusError>

// Re-undefine emit — the Qt headers above re-define it.
#ifdef emit
#undef emit
#endif

#include "bellfeedback.h"
#include "ghosteeladapter.h"
#include "kittyimagedecoder.h"

static void loadTranslations(QCoreApplication *app)
{
    QLocale locale = QLocale::system();
    auto *translator = new QTranslator(app);
    QString transDir = SailfishApp::pathTo("translations").toLocalFile();
    if (translator->load(locale.name(), "ghosteel", "_", transDir)) {
        app->installTranslator(translator);
    } else {
        // Try language-only (e.g. "de" from "de_DE")
        QString lang = locale.name().left(locale.name().indexOf('_'));
        if (!lang.isEmpty() && translator->load(lang, "ghosteel", "_", transDir)) {
            app->installTranslator(translator);
        } else {
            delete translator;
        }
    }
}

int main(int argc, char *argv[])
{
    qmlRegisterType<TerminalView>(APP_QML_MODULE, 1, 0, "TerminalView");
    qmlRegisterType<GLRenderer>(APP_QML_MODULE, 1, 0, "GLRenderer");

    // Register PNG decoder for Kitty Graphics Protocol (process-global, once)
    kittyImageDecoderRegister();

    // Parse CLI arguments for -e/--exec and -s/--session before the
    // single-instance check.  QCommandLineParser requires QCoreApplication,
    // which doesn't exist yet, so scan argv manually.
    QString execCommand;
    QStringList execArgs;
    QString sessionName;

    // -e/--exec consumes all remaining args (must be the last ghosteel option).
    // -s/--session takes one value.  A bare '--' marks everything after it as
    // the command verbatim, for commands that carry their own flags.
    for (int i = 1; i < argc; i++) {
        QString arg = QString::fromLocal8Bit(argv[i]);

        if (arg == QStringLiteral("-h") || arg == QStringLiteral("--help")) {
            printf("Usage: ghosteel [-s|--session <name>] [-e|--exec <command> [args...]]\n"
                   "       ghosteel [-s|--session <name>] -- <command> [args...]\n"
                   "\n"
                   "  -s, --session <name>            Switch to or create named session\n"
                   "  -e, --exec <command> [args...]  Run command instead of default shell\n"
                   "                                  (must be the last ghosteel option)\n"
                   "  --                              Treat the rest as the command verbatim,\n"
                   "                                  for commands with their own flags\n"
                   "  -h, --help                      Show this help\n"
                   "\n"
                   "Examples:\n"
                   "  ghosteel -s sysmon -e top\n"
                   "  ghosteel -s sysmon -- htop -s PERCENT_CPU\n");
            fflush(stdout);
            return 0;
        } else if (arg == QStringLiteral("--")) {
            if (i + 1 < argc) {
                execCommand = QString::fromLocal8Bit(argv[i + 1]);
                for (int j = i + 2; j < argc; j++)
                    execArgs.append(QString::fromLocal8Bit(argv[j]));
            }
            break;
        } else if (arg == QStringLiteral("-e") || arg == QStringLiteral("--exec")) {
            if (i + 1 < argc) {
                const QString cmd = QString::fromLocal8Bit(argv[i + 1]);
                if (cmd.startsWith(QLatin1Char('-'))) {
                    fprintf(stderr,
                            "ghosteel: -e requires a command, but the next token is '%s'.\n"
                            "ghosteel: Put ghosteel options before -e, or use '--' to give the\n"
                            "ghosteel: command its own flags, e.g.  ghosteel -s <name> -- <cmd> [args...]\n",
                            qPrintable(cmd));
                    return 1;
                }
                execCommand = cmd;
                for (int j = i + 2; j < argc; j++)
                    execArgs.append(QString::fromLocal8Bit(argv[j]));
                break;
            } else {
                fprintf(stderr, "ghosteel: -e requires a command argument\n");
                return 1;
            }
        } else if (arg == QStringLiteral("-s") || arg == QStringLiteral("--session")) {
            if (i + 1 < argc) {
                sessionName = QString::fromLocal8Bit(argv[i + 1]);
                i++;
            } else {
                fprintf(stderr, "ghosteel: -s requires a session name\n");
                return 1;
            }
        }
    }

    // Single-instance guard: if another instance is running, send the
    // appropriate IPC message and exit.  This handles D-Bus activation
    // launching a duplicate when a sandboxed instance is already running.
    if (SessionManager::checkSingleInstance(execCommand, execArgs, sessionName))
        return 0;

    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    loadTranslations(app.data());
    QScopedPointer<QQuickView> view(SailfishApp::createView());

    view->rootContext()->setContextProperty(QStringLiteral("Settings"), Settings::instance());

    SessionManager *sessionManager = new SessionManager(app.data());
    view->rootContext()->setContextProperty(QStringLiteral("SessionManager"), sessionManager);

    BellFeedback *bellFeedback = new BellFeedback(app.data());
    view->rootContext()->setContextProperty(QStringLiteral("bellFeedback"), bellFeedback);

    if (!execCommand.isEmpty() || !sessionName.isEmpty())
        sessionManager->setCliArgs(execCommand, execArgs, sessionName);

    // Register D-Bus adaptor for notification action callbacks.
    // Under Sailjail, D-Bus name ownership is restricted, so registration
    // may fail silently — this is expected and notification actions will
    // simply not work when sandboxed.  When launched via D-Bus activation
    // (the .service file), registration succeeds and actions work.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        if (bus.registerService(QStringLiteral("com.zackslash.ghosteel"))) {
            // Create adaptor as child of sessionManager — must exist before registerObject
            new GhosteelAdapter(sessionManager);
            if (bus.registerObject(QStringLiteral("/com/zackslash/ghosteel"), sessionManager)) {
                qDebug() << "Ghosteel: D-Bus service registered";
                sessionManager->setDbusRegistered(true);
            } else {
                qWarning() << "Ghosteel: D-Bus object registration failed:" << bus.lastError().message();
                bus.unregisterService(QStringLiteral("com.zackslash.ghosteel"));
            }
        } else {
            // Expected under Sailjail — dbus-user.own is restricted
            qDebug() << "Ghosteel: D-Bus service not registered (expected under Sailjail):" << bus.lastError().message();
        }
    } else {
        qDebug() << "Ghosteel: D-Bus session bus not available";
    }

    // Start single-instance socket server so future D-Bus activations
    // can detect this instance instead of spawning a duplicate.
    // Returns false when a duplicate was detected during the startup race
    // window; its CLI args were already forwarded. Returning before
    // setSource is intended — no UI was shown.
    if (!sessionManager->startSingleInstanceServer()) return 0;

    // Expose version strings to QML (always defined via -D flags from ghosteel.pro)
    view->rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(GIT_VERSION));
    view->rootContext()->setContextProperty(QStringLiteral("ghosttyVersion"), QStringLiteral(GHOSTTY_VERSION));
    view->rootContext()->setContextProperty(QStringLiteral("appName"), QStringLiteral(APP_NAME));

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
