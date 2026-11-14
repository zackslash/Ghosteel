// Include terminalview.h BEFORE Qt headers to ensure Ghostty's C headers
// are parsed before Qt defines the `emit` macro (which conflicts with
// Ghostty's use of 'emit' as a struct field name).
#include "terminalview.h"
#include "fontcatalog.h"
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
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#ifdef GHOSTEEL_STORE
#include <QDBusInterface>
#include <QDBusMessage>
#endif

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
#ifdef GHOSTEEL_STORE
    // SailfishApp::pathTo() derives its base from the QCoreApplication app
    // name (ghosteelstore), which does not match the install dir
    // (/usr/share/harbour-ghosteel) — resolve explicitly.
    QString transDir = QStringLiteral("/usr/share/" APP_NAME "/translations");
#else
    QString transDir = SailfishApp::pathTo("translations").toLocalFile();
#endif
    // QLocale overload: builds ghosteel_<locale>.qm and falls back through
    // shorter locale names (de_DE -> de) on its own.
    if (translator->load(locale, "ghosteel", "_", transDir)) {
        app->installTranslator(translator);
    } else {
        delete translator;
    }
}

#ifdef GHOSTEEL_STORE
// Seed the persistent child-shell home. Sailjail only persists the app's
// XDG dirs; the real $HOME the child would see is an empty read-only
// overlay whose writes are discarded. HOME for the shell points here, so
// dotfiles, .ssh and tool state survive across launches.
static void ensureSandboxHome()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/home");
    const QString skeleton = QStringLiteral("/usr/share/" APP_NAME "/skeleton/.profile");
    QDir().mkpath(home + QStringLiteral("/bin"));
    QDir().mkpath(home + QStringLiteral("/.tmp"));
    const QString profile = home + QStringLiteral("/.profile");
    if (!QFile::exists(profile) && QFile::exists(skeleton))
        QFile::copy(skeleton, profile);
}
#endif

// Q_DECL_EXPORT: exposes main() for the mapplauncherd Silica booster and
// the Harbour RPM validator's symbol check.
#ifndef GHOSTEEL_STORE
// One-time scrollback migration: the explicit org/app names move
// AppDataLocation from ~/.local/share/ghosteel to
// ~/.local/share/com.zackslash/ghosteel. Settings migrate on their own;
// scrollback files do not — copy them across once so existing users keep
// their session history.
static void migrateLegacyScrollback()
{
    const QString legacy = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/ghosteel/scrollback");
    const QString target = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           + QStringLiteral("/scrollback");
    if (QDir(target).exists() || !QDir(legacy).exists())
        return;
    if (!QDir(target).mkpath(QStringLiteral(".")))
        return;
    const auto files = QDir(legacy).entryInfoList({QStringLiteral("session_*.vt")}, QDir::Files);
    for (const QFileInfo &f : files)
        QFile::copy(f.absoluteFilePath(), target + QStringLiteral("/") + f.fileName());
}
#endif

Q_DECL_EXPORT int main(int argc, char *argv[])
{
    // App identity for QStandardPaths: the sandbox whitelist is derived from
    // the desktop file's OrganizationName/ApplicationName, and QStandardPaths
    // otherwise falls back to the binary name (harbour-ghosteel in the store
    // build), which does not match the whitelisted ApplicationName, so the
    // derived dirs would land outside the sandbox whitelist. Set explicitly
    // before anything touches Settings or AppDataLocation.
    QCoreApplication::setOrganizationName(QStringLiteral(APP_ORG));
    QCoreApplication::setApplicationName(QStringLiteral(APP_XDG_NAME));

    qmlRegisterType<TerminalView>(APP_QML_MODULE, 1, 0, "TerminalView");
    qmlRegisterType<GLRenderer>(APP_QML_MODULE, 1, 0, "GLRenderer");

    // Register PNG decoder for Kitty Graphics Protocol (process-global, once)
    kittyImageDecoderRegister();

    // Parse CLI arguments for -e/--exec, -s/--session and -r/--restart before the
    // single-instance check.  QCommandLineParser requires QCoreApplication,
    // which doesn't exist yet, so scan argv manually.
    QString execCommand;
    QStringList execArgs;
    QString sessionName;
    bool restart = false;

    // -e/--exec consumes all remaining args (must be the last ghosteel option).
    // -s/--session takes one value.  A bare '--' marks everything after it as
    // the command verbatim, for commands that carry their own flags.
    for (int i = 1; i < argc; i++) {
        QString arg = QString::fromLocal8Bit(argv[i]);

        if (arg == QStringLiteral("-h") || arg == QStringLiteral("--help")) {
            printf("Usage: ghosteel [-s|--session <name>] [-r] [-e|--exec <command> [args...]]\n"
                   "       ghosteel [-s|--session <name>] [-r] -- <command> [args...]\n"
                   "\n"
                   "  -s, --session <name>            Switch to or create named session\n"
                   "  -r, --restart                   With -e or --: replace an already-running\n"
                   "                                  matching session instead of switching to\n"
                   "                                  it, so the command runs every time\n"
                   "  -e, --exec <command> [args...]  Run command instead of default shell\n"
                   "                                  (must be the last ghosteel option)\n"
                   "  --                              Treat the rest as the command verbatim,\n"
                   "                                  for commands with their own flags\n"
                   "  -h, --help                      Show this help\n"
                   "\n"
                   "Examples:\n"
                   "  ghosteel -s sysmon -e top\n"
                   "  ghosteel -r -s sysmon -e top\n"
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
                const QString name = QString::fromLocal8Bit(argv[i + 1]);
                if (name.startsWith(QLatin1Char('-'))) {
                    fprintf(stderr,
                            "ghosteel: -s requires a session name, but the next token is '%s'.\n"
                            "ghosteel: Put ghosteel options before -e, or use '--' to give the\n"
                            "ghosteel: command its own flags, e.g.  ghosteel -s <name> -- <cmd> [args...]\n",
                            qPrintable(name));
                    return 1;
                }
                sessionName = name;
                i++;
            } else {
                fprintf(stderr, "ghosteel: -s requires a session name\n");
                return 1;
            }
        } else if (arg == QStringLiteral("-r") || arg == QStringLiteral("--restart")) {
            restart = true;
        }
    }

    // Single-instance guard: if another instance is running, send the
    // appropriate IPC message and exit.  This handles D-Bus activation
    // launching a duplicate when a sandboxed instance is already running.
    if (SessionManager::checkSingleInstance(execCommand, execArgs, sessionName, restart))
        return 0;

    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    loadTranslations(app.data());
#ifdef GHOSTEEL_STORE
    ensureSandboxHome();
#else
    migrateLegacyScrollback();
#endif
    QScopedPointer<QQuickView> view(SailfishApp::createView());

    view->rootContext()->setContextProperty(QStringLiteral("Settings"), Settings::instance());

    FontCatalog *fontCatalog = new FontCatalog(app.data());
    view->rootContext()->setContextProperty(QStringLiteral("FontCatalog"), fontCatalog);

    SessionManager *sessionManager = new SessionManager(app.data());
    view->rootContext()->setContextProperty(QStringLiteral("SessionManager"), sessionManager);

    BellFeedback *bellFeedback = new BellFeedback(app.data());
    view->rootContext()->setContextProperty(QStringLiteral("bellFeedback"), bellFeedback);

    if (!execCommand.isEmpty() || !sessionName.isEmpty())
        sessionManager->setCliArgs(execCommand, execArgs, sessionName, restart);

    // Register D-Bus adaptor for notification action callbacks.
    // The store build relies on service-name ownership as its single-instance
    // mechanism: the session bus is shared across Sailjail sandboxes through
    // the D-Bus proxy (local sockets are not), so a fresh process from an
    // icon re-tap or an unsandboxed CLI launch finds the name taken, forwards
    // its CLI args to the primary and exits.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        if (bus.registerService(QStringLiteral(APP_DBUS_NAME))) {
            // Create adaptor as child of sessionManager — must exist before registerObject
            new GhosteelAdapter(sessionManager);
            if (bus.registerObject(QStringLiteral("/com/zackslash/ghosteel"), sessionManager)) {
                qDebug() << "Ghosteel: D-Bus service registered";
                sessionManager->setDbusRegistered(true);
            } else {
                qWarning() << "Ghosteel: D-Bus object registration failed:" << bus.lastError().message();
                bus.unregisterService(QStringLiteral(APP_DBUS_NAME));
            }
        } else {
#ifdef GHOSTEEL_STORE
            // A primary instance owns the name. Forward the CLI request and
            // exit; if the method call fails (e.g. talk rule not granted),
            // continue as a duplicate rather than swallow the launch.
            QDBusInterface primary(QStringLiteral(APP_DBUS_NAME),
                                   QStringLiteral("/com/zackslash/ghosteel"),
                                   QStringLiteral("com.zackslash.ghosteel"));
            // Fail fast: a wedged primary must not freeze this launch on the
            // 25 s default D-Bus timeout — the duplicate fallback covers it.
            primary.setTimeout(2500);
            QDBusMessage reply = primary.call(QStringLiteral("launch"),
                                              execCommand, execArgs,
                                              sessionName, restart);
            if (reply.type() == QDBusMessage::ReplyMessage) {
                qDebug() << "Ghosteel: forwarded launch to primary; duplicate exiting";
                return 0;
            }
            qWarning() << "Ghosteel: primary forward failed; continuing as duplicate:"
                       << reply.errorMessage();
#else
            qDebug() << "Ghosteel: D-Bus service not registered:" << bus.lastError().message();
#endif
        }
    } else {
        qDebug() << "Ghosteel: D-Bus session bus not available";
    }

    // Start single-instance socket server so later launches (D-Bus
    // activations in the full build, failed-forward duplicates in the store
    // build, same-sandbox CLI calls) can detect this instance instead of
    // spawning a duplicate.
    // Returns false when a duplicate was detected during the startup race
    // window; its CLI args were already forwarded. Returning before
    // setSource is intended — no UI was shown.
    if (!sessionManager->startSingleInstanceServer()) return 0;

    // Expose version strings to QML. The libghostty version is reported by
    // the linked library itself (baked into libghostty-vt.a at build time),
    // so it always matches the binary actually shipped.
    view->rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(GIT_VERSION));
    view->rootContext()->setContextProperty(QStringLiteral("libghosttyVersion"), QString::fromUtf8(ghostty_version()));
    view->rootContext()->setContextProperty(QStringLiteral("appName"), QStringLiteral(APP_NAME));
#ifdef GHOSTEEL_STORE
    view->rootContext()->setContextProperty(QStringLiteral("sandboxedBuild"), true);
    view->setSource(QUrl::fromLocalFile(
        QStringLiteral("/usr/share/" APP_NAME "/qml/ghosteel.qml")));
#else
    view->rootContext()->setContextProperty(QStringLiteral("sandboxedBuild"), false);
    view->setSource(SailfishApp::pathToMainQml());
#endif
    view->show();

    return app->exec();
}
